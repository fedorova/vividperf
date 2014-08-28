/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2013 Intel Corporation. All rights reserved.
 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */

#include <assert.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string.h>
#include <string>
#include <sys/time.h>
#include <sstream> 
#include <map>
#include <utility>
#include "pin.H"

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
using namespace std;


PIN_LOCK lock;
ofstream traceFile;
bool LOUD = false;
bool go = false;

#define BITS_PER_BYTE 8

/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "memtracker.out", "specify trace file name");

KNOB<string> KnobTrackedFuncsFile(KNOB_MODE_WRITEONCE, "pintool",
    "f", "memtracker.in", "specify filename with procedures "
			   "where you want to track memory accesses");

KNOB<string> KnobAllocFuncsFile(KNOB_MODE_WRITEONCE, "pintool",
    "a", "alloc.in", "specify filename with procedures performing "
			   "memory allocations");

KNOB<int> KnobAppPtrSize(KNOB_MODE_WRITEONCE, "pintool",
				"p", "64", "application pointer size in bits (default is 64)");

KNOB<bool> KnobWithGDB(KNOB_MODE_WRITEONCE, "pintool", "g", "false", 
		       "Are we running with the concurrent GDB session to find allocated types or not?");


/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
   
INT32 Usage()
{
    cerr << "This tool produces a trace of calls to a function." << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}


/* ===================================================================== */

/* We keep track of two types of functions: 
 *   - Optionally, the functions the user wants us to track for memory accesses.
 *     By default we keep track of accesses in the entire program. 
 *   - *alloc functions that perform memory allocations. We need those so we
 *     can record the addresses allocated on the heap and call sites of those
 *     allocations, so we can later resolve the type and contents of the
 *     memory location. 
 */
vector<string> TrackedFuncsList;
vector<string> AllocFuncsList;

/* This struct describes the prototype of an alloc function 
 * Fiolds "number", "size" and "retaddr" tell us which argument (first argument is indexed '0')
 * gives us the number of elements to allocate, the allocation size and the
 * return address pointer. If we always allocate one item of the given size
 * (as in malloc), the field "number" should be "-1". If the function returns the address
 * of the allocation as opposed to passing it to the application in the in-out
 * pointer, then field "retaddr" should be set to -1.
 */
typedef struct func_proto
{
    string name;
    int number;
    int size;
    int retaddr;
    vector<struct func_proto*> otherFuncProto;
} FuncProto;

vector<FuncProto *> funcProto;

/* This data structure contains the information about this allocation
 * that we must remember between the time we enter the allocation function
 * and return from it. 
 * Since multiple threads might be making calls to alloc at the same time, 
 * we keep this structure per-thread. 
 */

typedef struct thread_alloc_data
{
    ADDRINT calledFromAddr;
    int line;
    int column;
    ADDRINT size;
    int number;
    ADDRINT addr;
    ADDRINT retptr;
    string filename;
    string varName;
} ThreadAllocData;

typedef struct source_location
{
    string filename;
    int line;
    string varname;
} SourceLocation;

typedef struct func_record
{
    string name;
    int breakID;
    int retaddr;
    bool noSourceInfo;
    vector<FuncProto *> otherFuncProto;
    map<ADDRINT, SourceLocation*> locationCache;
    vector<ThreadAllocData*> thrAllocData; 
} FuncRecord;

vector<FuncRecord*> funcRecords;
unsigned int largestUnusedThreadID = 0;

string GDB_CMD_PFX = "gdb: ";

/* ===================================================================== */
/* Helper routines                                                       */
/* ===================================================================== */
FuncRecord* findFuncRecord(vector<FuncRecord*> *frlist, string name)
{
    assert(lock._owner != 0);

    for(FuncRecord* fr: *frlist)
    {
	if(fr->name.compare(name) == 0)
	    return fr;
    }

    return NULL;
}

FuncRecord* allocateAndAdd(vector<FuncRecord*> *frlist, 
			   FuncProto* fp)
{
    assert(lock._owner != 0);

    FuncRecord *fr = new FuncRecord();
    assert(fr != NULL);
    
    frlist->push_back(fr);
    
    fr->name = fp->name; 
    fr->retaddr = fp->retaddr;
    fr->otherFuncProto = fp->otherFuncProto;

    for(uint i = 0; i < largestUnusedThreadID; i++)
    {
	ThreadAllocData *tr = new ThreadAllocData();

	/* Zero out all the POD, but don't touch the C++ string */
	memset(tr, 0, offsetof(ThreadAllocData, filename));
	fr->thrAllocData.push_back(tr);
    }

    return fr;
}

VOID trim(string& s)
{
    
    /* Trim whitespace from the beginning */
    bool reachedBeginning = false;
    while(!reachedBeginning)
    {
	const char *cstring = s.c_str();
	
	if(isspace(cstring[0]))
	    s.erase(0, 1);
	else
	    reachedBeginning = true;
    }

    /* Trim whitespace from the end */
    bool reachedEnd = false;
    while(!reachedEnd)
    {
	const char *cstring = s.c_str();
	int len = s.length();

	if(isspace(cstring[len-1]))
	    s.erase(len-1, 1);
	else
	    reachedEnd = true;
    }
}


/* 
 * We are given a part of the source line that begins with the name of the variable.
 * Let's trim the substring to get rid of any characters that are not part of the 
 * variable name. Characters that can be a part of the variable name are letters, 
 * numbers and the underscore. We also allow "-" and ">" in case we have a complex
 * variable that's dereferencing a pointer, and "[" and "]" in case we have an array.
 */
VOID trimVarName(string& var)
{
    unsigned int pos = 0;

    while(pos < var.length())
    {
	const char *cstring = var.c_str();
	
	if(!isalnum(cstring[pos]) && cstring[pos] != '_' 
	   && cstring[pos] != '-' && cstring[pos] != '>'
	   && cstring[pos] != '[' && cstring[pos] != ']')
	    var.erase(pos, 1);
	else
	    pos++;
    }
}

/*
 * Parse the prototypes of the allocation functions we are tracking.
 * Each function is on its own line. 
 * The first word on the line is the name of the function. 
 * 
 * The second token is an integer specifying number of the argument that
 * tells us how many items we are allocating (as in calloc) or "-1" if 
 * the function does not take such an argument. 
 *
 * The third token is an integer specifying which argument (0th, 1st, 2nd) 
 * tells us the size of the allocated item. This value can never be "-1".
 *
 * The fourth token token is an integer specifying which argument (if any) contains
 * the return address of the allocated memory chunk.
 * This token can either be "-1" or a positive integer. If it is "-1" then the address
 * of the allocated memory chunk (a.k.a. return pointer) is returned by the
 * alloc function (as in malloc). Otherwise, the integer tells us which
 * of the alloc function argument (0th, 1st, 2nd, etc.) contains the pointer to the 
 * allocated address.
 */

VOID parseAllocFuncsProto(vector<string> funcs)
{

    for(string funcDef: funcs)
    {
	bool subDef = false;

	if(funcDef.empty())
	    continue;

	/* Trim whitespace */
	trim(funcDef);

	/* If a line starts with a "!", it's a sub-definition, 
	 * meaning that this is an alternative definition for
	 * a function defined in the previous line. This 
	 * can happen if the function is wrapped in a macro.
	 * In that case, the instrumentation will fire when
	 * we execute the actual function, but the source code
	 * location will guide us to where we are calling the
	 * macro, so we need to know the macro's prototype in
	 * order to correctly parse the name of the allocated
	 * variable. 
	 */
	if(funcDef.find("!") == 0)
	{
	    subDef = true;
	    funcDef = funcDef.substr(1);
	}

	istringstream str(funcDef);
	int iter = 0;
	
	FuncProto *fp = new FuncProto();

	while(!str.eof())
	{
	    string word;
	    str >> word;

	    switch(iter)
	    {
	    case 0:
		fp->name = word;
		break;
	    case 1:
		fp->number = atoi(word.c_str());
		break;
	    case 2:
		fp->size = atoi(word.c_str());
		break;
	    case 3:
		fp->retaddr = atoi(word.c_str());
		break;
	    case 4:
		cerr << "Invalid format in alloc.in file. Expecting the function "
		    "name and three numbers for prototype (see help message)." 
		     << endl;
		Usage();
		exit(-1);
	    }
	    iter++;
	}
	if(iter != 4)
	{
	    cerr << "Invalid format in alloc.in file. Expecting the function "
		"name and three numbers for prototype (see help message)." 
		 << endl;
	    Usage();
	    exit(-1);
	}
	else
	{
	    if(!subDef)
		funcProto.push_back(fp);
	    else
	    {
		FuncProto *last = funcProto.back();
		if(last == NULL)
		{
		    cerr << "Format error in alloc.in file. "
			"Sub-definition (line starting with \"!\") provided, "
			"but not preceeded by a regular function definition "
			"line (no \"!\" in the beginning)." << endl;
		    exit(-1);
		}
		last->otherFuncProto.push_back(fp);
		cout << last->name << " has alternative function "
		    "prototype under name " << fp->name << endl;
	    }
	}	
    }
}
	   
const char * StripPath(const char * path)
{
    const char * file = strrchr(path,'/');
    if (file)
        return file+1;
    else
        return path;
}

#define BILLION 1000000000


bool fileError(ifstream &sourceFile, string file, int line)
{
    if(!sourceFile)
    {
	cerr << "Error parsing file " << file << endl;
	if(sourceFile.eof())
	    cerr << "Reached end of file before reaching line "
		 << line << endl;
	else
	    cerr << "Unknown I/O error " << endl;
	return true;
    }
    else
	return false;
}

/* We need to find the function name and make
 * sure that it's followed either by a newline or
 * space or "(".
 */
bool functionFound(string line, string func, size_t *pos)
{
    if((*pos = line.find(func)) != string::npos
       &&
       (*pos + func.length() == line.length() ||
	line.c_str()[*pos+func.length()] == '(' ||
	isspace(line.c_str()[*pos+func.length()])))
	return true;
    else
    {
	*pos = string::npos;
	return false;
    }
}

/*
 * This routine will parse the file and return to us the
 * name of the variable, for which we are allocating space. 
 * This variable might be passed to the function as a pointer,
 * which holds the allocated address or it can be assigned the
 * return value of the function. The value of "arg" will tell us
 * which situation we are dealing with. If its the former, arg
 * will tell us which argument is the allocated variable. If it's
 * the latter, arg will equal to "-1". 
 *
 * We pass the full path to the file name and the line within
 * that file (this is the location from where the alloc function
 * was called), the name of the alloc function, and "arg" (described
 * above)
 */
string findAllocVarName(string file, int line, string func, int arg,
			vector<FuncProto*>otherFuncProto)
{
    ifstream sourceFile;
    string lineString;
    size_t pos;
    int filepos = line;

    sourceFile.open(file.c_str());
    if(!sourceFile.is_open())
    {
	cerr << "Failed to open file " << file << endl;
	cerr << "Cannot parse allocated type " << endl;
	return "";
    }

    while(filepos-- > 0)
    {
	getline(sourceFile, lineString);
	if(fileError(sourceFile, file, line))
	   return "";	   
    }

#if 0
    /* Ok, here is the line we want to parse */
    cout << "Will parse: "<< endl;
    cout << lineString << endl;
#endif

    /* First let's find the alloc function name on that line.
     * We need to figure out whether this is the original 
     * function definition or one of its "other" prototypes, 
     * because this will determine which argument (or return value)
     * holds the allocated address.
     */
    if(!functionFound(lineString, func, &pos))
    {
	/* Let's go through alternative function
	 * prototypes and find it.
	 */
	for(FuncProto *fp: otherFuncProto)
	{
	    if(functionFound(lineString, fp->name, &pos))
	    {
		arg = fp->retaddr;
#if 0
		cout << "Detected alternate prototype with name "
		     << fp->name << ", arg is: " << arg << endl;
#endif
		break;
	    }
	}
    }

    if(pos == string::npos)
    {
	cerr << "Cannot find func name " << func << 
	    " on line " << line << " in file " << file << endl;
	return "";
    }
    
    /* If the allocated variable is an argument to the function 
     * let's skip to it, otherwise, if it is assigned the return
     * value of alloc, let's back-off to find it. 
     */
    if(arg == -1) /* Variable is the return value. Back off */
    {
	/* TODO */
	cerr << "Bumped into unimplemented functionality " << endl;
    }
    else
    {
	assert(arg >= 0);

	/* We are skipping lines until we find an opening
	 * bracket after the function name, because 
	 * the function invocation may be spread across
	 * several lines. 
	 */
	while( (pos = lineString.find("(", pos)) == string::npos)
	{
	    getline(sourceFile, lineString);
	    pos = 0;
	    if(fileError(sourceFile, file, line))
		return "";
	}
	
	/* Ok, now from the opening bracket, we need to skip
	 * to the arg-th argument, assuming the first argument is
	 * arg 0. So skip as many commas as necessary
	 */
	int commasToSkip = arg;
#if 0
	cout << "Skipping " << commasToSkip << " commas " << endl;
#endif
	while(commasToSkip-- > 0)
	{
	    /* Here again we may have to skip lines, because
	     * the function invocation may be spread across
	     * multiple lines. 
	     */
	    while( (pos = lineString.find(",", pos+1)) == string::npos)
	    {
		getline(sourceFile, lineString);
		pos = 0;
		if(fileError(sourceFile, file, line))
		    return "";
#if 0
		cout << "Skipped line to:" << endl;
		cout << lineString << endl;
#endif
	    }
#if 0
	    cout << commasToSkip << " commas left " << endl;
#endif
	}
	/* We've skipped to the last comma, now let's skip
	 * over it.
	 */
	pos++;

	/* Ok, we've skipped all the commas. Now let's skip 
	 * over the whitespace, which may span multiple lines,
	 * to get to our variable.
	 */
	size_t endpos = lineString.length();

	/* We've reached the end of line after skipping all commas.
	 * Our variable must be on the next line.  
	 */
	if(pos == endpos)
	{
	    getline(sourceFile, lineString);
	    if(fileError(sourceFile, file, line))
		return "";

	    pos = 0;
	    endpos = lineString.length();
	}

	while(pos <= endpos && isspace(lineString.c_str()[pos]))
	{
	    /* We've reached the end of line */
	    if(pos == endpos)
	    {
		getline(sourceFile, lineString);
		if(fileError(sourceFile, file, line))
		    return "";

		pos = 0;
		endpos = lineString.length();
	    }
	    else
		pos++;
	}

#if 0
	cout << lineString ;
	cout << "Pos is: " << pos << endl;
	cout << "Found variable in substring: " << lineString.substr(pos)
	     << endl;
#endif
    }

    /* Let's trim away all foreign characters from the line
     * so all we are left with is the name of the variable 
     */
    string var = lineString.substr(pos);
    trimVarName(var);
    
    if(LOUD)
	cout << "Var name is: " << var << endl;

    sourceFile.close();
    return var;
}


/* ===================================================================== */
/* Analysis routines                                                     */
/* ===================================================================== */


VOID callBeforeAlloc(FuncRecord *fr, THREADID tid, ADDRINT addr, ADDRINT number, 
		     ADDRINT size, ADDRINT retptr)
{
    INT32 column = 0, line = 0;
    string filename = ""; 
    string varname;

    /* We were called before the application has begun running main.
     * This can happen for malloc calls. Don't do anything. But tell the
     * python GDB driver to let us continue. 
     */
    if(!go)
	return;


    if(LOUD)
    {
	cout << "----> BEFORE ALLOC(" << tid << ")" << fr->name 
	     << ". Return address is: " << hex << addr << dec << endl;
    }

    assert(fr->thrAllocData.size() > tid);

    if(fr->thrAllocData[tid]->calledFromAddr != 0)
	cout << "Warning: recursive allocation: " << fr->name <<
	    ", retaddr: " << hex << addr << dec << ", size: " << size << endl;

    /* This check for noSourceInfo can race, but in the worst case, 
     * we'll just do a little bit extra work. 
     */
    if(!fr->noSourceInfo)
    {
	PIN_GetLock(&lock, PIN_ThreadId() + 1);

	map<ADDRINT, SourceLocation*>::iterator it = 
	    fr->locationCache.find(addr);

	if(it == fr->locationCache.end())
	{
	    if(LOUD)
		cout << "Location " << hex << addr << dec << 
		    " not cached" << endl;

	    PIN_LockClient();
	    PIN_GetSourceLocation(addr, &column, &line, &filename);
	    PIN_UnlockClient();
	    
	    SourceLocation *sloc = new SourceLocation();
	    sloc->filename = filename;
	    sloc->line = line;


	    /* If we are getting an empty string, then this means
	     * that this alloc function has no debug information
	     * associated with it, so there is nothing we can do
	     * to figure out the type of the allocated object. 
	     * Disable the breakpoint, so it doesn't slow us down.
	     */
	    if(filename.length() == 0)
	    {
		fr->noSourceInfo = true;
		sloc->varname = "unknown";

		if(KnobWithGDB)
		{
		    if(LOUD)
			cout << "Disabling breakpoint " << fr->breakID 
			     << " in " << fr->name << endl << 
			    "No debug information is present " << endl;

		    cout << GDB_CMD_PFX << "disable " << fr->breakID << endl;
		}
	    }
	    else if(filename.length() > 0 && line > 0)
	    {
		varname = 
		    findAllocVarName(filename, line, fr->name, fr->retaddr,
				     fr->otherFuncProto);

		if(KnobWithGDB)
		{
		    /* Ask about the varType from GDB */
		    cout << GDB_CMD_PFX << "finish " << endl;
		    cout << GDB_CMD_PFX << "whatis " << varname << endl;    
		}

		sloc->varname = varname;
	    }

	    /* Insert this record into the cache */
	    fr->locationCache.insert(make_pair(addr, sloc));
		    
	}
	else {

	    filename = it->second->filename;
	    line = it->second->line;
	    varname = it->second->varname;

	    if(LOUD)
	    {
		cout << "Found " << hex << addr << dec << " in cache" << endl;
		cout << "Source location: " << filename << ":" << line << endl;
		cout << "Varname: " << varname << endl;
	    }

	}
	if(KnobWithGDB)
	    cout << GDB_CMD_PFX << "cont " << endl;    

	PIN_ReleaseLock(&lock);
    }


    fr->thrAllocData[tid]->calledFromAddr = addr;
    fr->thrAllocData[tid]->filename = filename;
    fr->thrAllocData[tid]->line = line;
    fr->thrAllocData[tid]->column = column;

    fr->thrAllocData[tid]->size = size;
    fr->thrAllocData[tid]->number = number;
    fr->thrAllocData[tid]->retptr = retptr;

    fr->thrAllocData[tid]->varName = varname;

    if(LOUD)
	cout << "<---- BEFORE ALLOC" << endl;

}

VOID callAfterAlloc(FuncRecord *fr, THREADID tid, ADDRINT addr)
{

    if(!go)
	return;

    if(LOUD)
	cout << "----> AFTER ALLOC " << fr->name << " (" << tid << ")" << endl; 

    cout.flush();

    assert(fr->thrAllocData.size() > tid);
    assert(fr->thrAllocData[tid]->calledFromAddr != 0);
    

    if(fr->thrAllocData[tid]->retptr == 0)
    {
	/* The address of the allocated chunk is the 
	   return value of the function */
	fr->thrAllocData[tid]->addr = addr;
    }
    else
    {
	/* The address of the allocated chunk is in the location pointed to 
	 * by the retptr argument we received as the argument to the function
	 */
	PIN_SafeCopy( &(fr->thrAllocData[tid]->addr), 
		      (const void*)fr->thrAllocData[tid]->retptr, 
		      KnobAppPtrSize/BITS_PER_BYTE);
    }

    if(LOUD)
	cout << "<---- AFTER ALLOC " << endl;

    /* Now, let's print the allocation record */
    cout << "alloc: 0x" << hex << setfill('0') << setw(16) 
	 << fr->thrAllocData[tid]->calledFromAddr
	 << dec << " " << fr->name << " " << tid 
	 << " 0x" << hex << setfill('0') << setw(16) << fr->thrAllocData[tid]->addr 
	 << dec 
	 << " " << fr->thrAllocData[tid]->size << " " 
	 << fr->thrAllocData[tid]->number 
	 << " " << fr->thrAllocData[tid]->filename 
	 << ":" << fr->thrAllocData[tid]->line
	 << " " << fr->thrAllocData[tid]->varName
	 << endl << endl; 

    /* Since we are exiting the function, let's reset the
     * "called from" address, to indicate that we are no longer
     * in the middle of the function call. 
     */
    fr->thrAllocData[tid]->calledFromAddr = 0;
}

/* 
 * Callbacks on alloc functions are not working properly before main() is called.
 * We sometimes get a callback for function exit without having received the callback
 * on function enter. So we don't track anything before main() is called. 
 */
VOID callBeforeMain()
{
    if(LOUD)
	cout << "MAIN CALLED ++++++++++++++++++++++++++++++++++++++++++" << endl;
    go = true;
}

/*
 * Callback functions to trace memory accesses.
 *
 */

// Print a memory write record
VOID recordMemoryRead(ADDRINT addr, UINT32 size)
{
    THREADID threadid  = PIN_ThreadId();
    PIN_GetLock(&lock, threadid+1);

    cout << "read: " << threadid << " 0x" << hex << setw(16) 
	 << setfill('0') << addr << dec << " " << size << endl;

    PIN_ReleaseLock(&lock);
}

// Print a memory write record
VOID recordMemoryWrite(ADDRINT addr, UINT32 size)
{
    THREADID threadid  = PIN_ThreadId();
    PIN_GetLock(&lock, threadid+1);

    cout << "write: " << threadid << " 0x" << hex << setw(16) 
	 << setfill('0') << addr << dec << " " << size << endl;

    PIN_ReleaseLock(&lock);
}

/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID Image(IMG img, VOID *v)
{
    static int lastBreakPointID = 1;

    /* Find main. We won't do anything before main starts. */
    RTN rtn = RTN_FindByName(img, "main");
    if(RTN_Valid(rtn))
    {
	RTN_Open(rtn);
	RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeMain, IARG_END);
	RTN_Close(rtn);
    }


    /* Go over all the allocation routines we are instrumenting and insert the
     * instrumentation.
     */
    for(FuncProto *fp: funcProto)
    {

	RTN rtn = RTN_FindByName(img, fp->name.c_str());

	if (RTN_Valid(rtn))
	{
	    FuncRecord *fr;
	    cout << "Procedure " << fp->name << " located." << endl;

	    PIN_GetLock(&lock, PIN_ThreadId() + 1);
	    if((fr = findFuncRecord(&funcRecords, fp->name)) == NULL)
	    {
		fr = allocateAndAdd(&funcRecords, fp);

		if(KnobWithGDB)
		{
		    /* Tell GDB to break on all alloc functions we are tracking. 
		     * We are simply printing to stdout with a special prefix. 
		     * A python script will parse that and pass everything after
		     * the prefix as the command to the debugger. 
		     */	
		    cout << GDB_CMD_PFX << "break " << fr->name << endl << flush;
		    cout << GDB_CMD_PFX << "commands " << endl << flush;
		    cout << GDB_CMD_PFX << "next " << endl << flush;
		    cout << GDB_CMD_PFX << "end " << endl << flush;	    

		    /* We need to remember the breakpoint ID associated with this
		     * function, because if we later learn that this function has
		     * no line number information, then we would need to disable
		     * this breakpoint. We know that the very first breakpoint we
		     * will have is in main() -- #1. So the remaining breakpoints
		     * will just be consecutive numbers after 1. 
		     */
		    fr->breakID = lastBreakPointID + 1;
		    lastBreakPointID++;
		    fr->noSourceInfo = 0;
		}
	    }


	    assert(fr != NULL);
	    
	    PIN_ReleaseLock(&lock);

	    RTN_Open(rtn);

	    // Instrument 
	    if(fp->number > 0 && fp->size > 0  && fp->retaddr > 0)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->number, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->retaddr, IARG_END);
	    }
	    else if(fp->number == -1 && fp->size > 0 && fp->retaddr > 0)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_ADDRINT, 1, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->retaddr, IARG_END);

	    }
	    else if(fp->number == -1 && fp->size >= 0 && fp->retaddr == -1)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_ADDRINT, 1, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_ADDRINT, 0, IARG_END);
	    }
	    else if(fp->number > 0 && fp->size > 0  && fp->retaddr == -1)
	    {
		RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeAlloc,
			       IARG_PTR, fr, IARG_THREAD_ID, IARG_RETURN_IP, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->number, 
			       IARG_FUNCARG_ENTRYPOINT_VALUE, fp->size,
			       IARG_ADDRINT, 0, IARG_END);

	    }
	    else {
		cerr << "I did not understand this function prototype: " << endl
		     << fp->name << ": number " << fp->number << ", size " << fp->size
		     << ", retaddr " << fp->retaddr << endl;
		Usage();
		exit(-1);
	    }


	    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)callAfterAlloc,
			   IARG_PTR, fr, IARG_THREAD_ID, IARG_FUNCRET_EXITPOINT_VALUE, 
			   IARG_END);

	    RTN_Close(rtn);

	}
    }
    
    /* Now let's go over all the routines where we want to track 
     * the actual memory accesses and instrument them.
     */
    for (string fname: TrackedFuncsList)
    {
	THREADID threadid;
	RTN rtn = RTN_FindByName(img, fname.c_str());

	if (!RTN_Valid(rtn))
	    continue;

	threadid = PIN_ThreadId();
	PIN_GetLock(&lock, threadid+1);
	cout << "Procedure " << fname << " located." << endl;

	RTN_Open(rtn);

	/* Insert instrumentation for each instruction in the routine */
	for(INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins))
	{
	    if (INS_IsMemoryWrite(ins))
	    {
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)recordMemoryWrite,
			       IARG_MEMORYWRITE_EA, 
			       IARG_MEMORYWRITE_SIZE, IARG_END);
	    }
	    if (INS_IsMemoryRead(ins))
	    {
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)recordMemoryRead,
			       IARG_MEMORYREAD_EA, 
			       IARG_MEMORYREAD_SIZE, IARG_END);
	    }

	}

	RTN_Close(rtn);
	PIN_ReleaseLock(&lock);
    }	    
}


/* ===================================================================== */
/* Parse the list of functions we want to instrument                    */
/* ===================================================================== */

VOID parseFunctionList(const char *fname, vector<string> &list)
{

    ifstream f;
    
    f.open(fname);
    
    cout << "Routines specified for instrumentation:" << endl;
    while(!f.eof())
    {
	string name;
	getline(f, name);
	cout << name << endl;
	
	/* Lines beginning with a # are to be ignored */
	if(name.find("#") == 0)
	    continue;
	list.push_back(name);
    }

    f.close();
}


/* ===================================================================== */

/* 
 * Go over all func records and allocate thread-local storage for that
 * data. That storage is only allocated, never freed. If a thread exits,
 * that thread's slot is simply unused. Pin thread IDs begin from zero and 
 * monotonically increase. The IDs are not reused, so each new thread needs
 * to allocate space for itself in those function records that have already
 * been created. Some function records can be created after threads have
 * started. In that case, thread storage will be allocated at the time of
 * creation of function records in the Image function. 
 *
 * Thread IDs are not necessarily consecutively increasing. We may see
 * a thread 0 followed by a thread 2, for instance. (I.e., this may happen
 * if we are running pin and debugging the app at the same time.). In order
 * to speed up the thread's access to its local data, we want to be able
 * to index the array of per-thread data records by thread id. So we
 * if we see a gap in thread IDs we will create an extra record in the
 * array to fill that gap, even though that record will never be used.  
 */

VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    PIN_GetLock(&lock, threadid+1);

    if(LOUD)
	cout << "Thread " << threadid << " is starting " << endl;

    /* Thread IDs are monotonically increasing and are not reused
     * if a thread exits. */
    largestUnusedThreadID = threadid+1;

    for(FuncRecord *fr: funcRecords)
    {
	while(fr->thrAllocData.size() < (threadid + 1))
	{

	    ThreadAllocData *tr = new ThreadAllocData();

	    /* Zero out all the pods, but don't touch the C++ string */
	    memset(tr, 0, offsetof(ThreadAllocData, filename));
	    fr->thrAllocData.push_back(tr);
	}
    }

    PIN_ReleaseLock(&lock);
}

VOID Fini(INT32 code, VOID *v)
{
    cout << "PR DONE" << endl;
    traceFile.close();
}



/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char *argv[])
{
    // Initialize pin & symbol manager
    PIN_InitSymbols();
    if( PIN_Init(argc,argv) )
    {
        return Usage();
    }    

    traceFile.open(KnobOutputFile.Value().c_str());

    parseFunctionList(KnobTrackedFuncsFile.Value().c_str(), TrackedFuncsList);
    parseFunctionList(KnobAllocFuncsFile.Value().c_str(), AllocFuncsList);
    parseAllocFuncsProto(AllocFuncsList);

    IMG_AddInstrumentFunction(Image, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_InitLock(&lock);

    if(KnobWithGDB)
	cout << "Assuming a concurrent GDB session " << endl;

    // Never returns
    PIN_StartProgram();
    
    return 0;
    
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */