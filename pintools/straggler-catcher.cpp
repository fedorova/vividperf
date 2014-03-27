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

 /*
  * This tool watches latencies of functions specified by the user and 
  * if any function registers a latency greater than X (where X is
  * the configuration parameter specified by the user), we invoke
  * the user-defined script. That scripts can do various things
  * to react to our catching of a long-latency spike: attach to the
  * program with the debugger, gather more stats, send an SMS to the
  * user, etc. 
  */

#include <assert.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string.h>
#include <sys/time.h>
#include <unordered_map>
#include <sstream> 
#include "pin.H"

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
using namespace std;

ifstream inputFile; 
PIN_LOCK lock;
UINT numAppThreads = 0;
BOOL scriptProvided = FALSE;
char* scriptCMDPartI;

/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobInputFile(KNOB_MODE_WRITEONCE, "pintool",
    "i", "stragglers.in", "specify filename with procedures to watch for high latency");

KNOB<string> KnobScriptPath(KNOB_MODE_WRITEONCE, "pintool",
    "s", "", "specify the full path of the script to invoke when we catch a straggler");

KNOB<UINT32> KnobTimeInterval(KNOB_MODE_WRITEONCE, "pintool",
    "t", "1000", "stragger catcher thread should check for stragglers every that many milliseconds");



/* ===================================================================== */

/* ===================================================================== */
/* Data Structures and helper routines */
/* ===================================================================== */

/* Holds names of functions the user wants us to track.
 * Not all of them will be valid. Validity will be checked on
 * image load, which is when valid functions will get an associated
 * function record and will get put into a hashmap. 
 */
typedef struct func_name
{
    string name;
    UINT64 threshold;
    struct func_name * next;
} FuncName;

FuncName * funcNameList = 0;


/* We will allocate an array with per-thread data assuming that we will have no more than
 * 64 threads. If we do have more than 64, we will reallocate the array to accommodate the
 * larger number. 
 */
unsigned int threadArraySize = 32;
unsigned int largestUnusedThreadID = 0;

#define CACHE_LINE_SIZE 64

typedef struct thread_local_data
{
    UINT64 timeAtLastEntry;
    UINT64 invCount;
    char valid;
    UINT8 padding[CACHE_LINE_SIZE-sizeof(UINT64)*2-sizeof(char)];
} ThrLocData; 

typedef struct func_record
{
    string name;
    UINT64 latencyThreshold;
    string image;
    ADDRINT address;
    ThrLocData *thrFuncRecords;
} FuncRecord;


/* This where we store function records keyed by the function name. 
 * We are storing the function name twice. Get rid of the duplicate if
 * this becomes a problem. 
 */
unordered_map<string, FuncRecord*> funcMap;

/* Useful for debugging. Must hold the lock when this function is called. */
VOID printAllRecords()
{
    cout << "---------------------------------------------" << endl;

    for (auto& x: funcMap) {
	std::cout << x.first << ": " << x.second << std::endl;
	FuncRecord *fr = (FuncRecord *) x.second;

	cout << fr->name << ", thr:" <<  fr->latencyThreshold << 
	    ", img: " << fr->image << hex << ", addr: " << fr->address << dec << endl;

	if(fr->thrFuncRecords){
	    UINT i;
	    
	    for(i = 0; i < largestUnusedThreadID; i++)
	    {
		ThrLocData *tld = &(fr->thrFuncRecords[i]);
		if(tld->valid)
		    cout << "Thread: " << i << ", "<< 
			"Time last entry: " << tld->timeAtLastEntry 
			 << ", invCount: " << tld->invCount << endl;
	    }
	}

	cout << "++++++++" << endl;    
    }
    cout << "---------------------------------------------" << endl;    


}


/* This means we have more threads than anticipated.
 * Reallocate a larger array and copy the data from the old array 
 * into the new one. Threads might be accessing the old array while
 * we are copying the data, so we may loose some stragglers. 
 * That's ok. We don't want to be acquiring locks in the critical
 * path, so we choose to lose data on these very rare occasions. 
 * We must be holding the lock while this function is called.
 */
int allocMoreSpaceAndCopy()
{
    unsigned int newsize = threadArraySize * 2;

    for (auto& x: funcMap) 
    {
    	FuncRecord *fr = x.second;
	assert(fr->thrFuncRecords);
	ThrLocData *newArray;

	newArray = (ThrLocData *)malloc(newsize * sizeof(ThrLocData));
	if(newArray == NULL)
	    return -1;

	memset((void*)newArray, 0, newsize * sizeof(ThrLocData));
	memcpy((void*)newArray, fr->thrFuncRecords, threadArraySize * sizeof(ThrLocData));
	
	fr->thrFuncRecords = newArray;
	threadArraySize = newsize;
	cout << "Reallocated space for " << fr->name << endl;
    }
    return 0;
}

/* We check if the thread-local record keeping space for each function 
 * we are tracking is there, and if not we allocate it. We find functions
 * we need to track on Image load, but sometimes a thread gets created after
 * the image is loaded. In that case, it needs to allocate the space for
 * itself. The lock must be held while we are manipulating the shared parts
 * of the function records.
 */
VOID markThreadRecValid(unsigned int threadid)
{
    assert(threadid < largestUnusedThreadID);
    if(largestUnusedThreadID > threadArraySize)
    {
	int err = allocMoreSpaceAndCopy();

	if(err)
	{
	    cerr<< "INSUFFICIENT THREAD-LOCAL STORAGE FOR " << largestUnusedThreadID 
		<< " THREADS. ABORTING..." << endl;
	    exit(-1);
	}
    }

    for (auto& x: funcMap) 
    {
    	FuncRecord *fr = x.second;
	assert(fr->thrFuncRecords);

	fr->thrFuncRecords[threadid].valid = 1;
    }
}

/* The reverse of markThreadRecValid. 
 * We mark the record invalid if the thread to whom the record belongs is exiting. 
 * That way we don't have to check the records of threads who are no longer running, 
 * and we don't waste time. 
 */
VOID markThreadRecInvalid(unsigned int threadid)
{
    assert(threadid < largestUnusedThreadID);

    for (auto& x: funcMap) 
    {
    	FuncRecord *fr = x.second;
	assert(fr->thrFuncRecords);

	fr->thrFuncRecords[threadid].valid = 0;
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
#define MILLION 1000000
#define THOUSAND 1000


/* ===================================================================== */
/* Analysis routines                                                     */
/* ===================================================================== */
 
/* This is what we do if we catch a straggler. */
inline VOID stragglerCaught(FuncRecord *fr, THREADID threadid, UINT64 elapsedTime)
{
    int ret;

    if(scriptProvided)
    {
	UINT maxlen = strlen(scriptCMDPartI) + strlen(fr->name.c_str()) + 55;
	char* scriptCMD = (char *) malloc(maxlen);
	if(scriptCMD == NULL)
	{
	    cerr << "Couldn't malloc " << endl;
	    exit(-1);
	}
	snprintf(scriptCMD, maxlen, "%s %s %ld\n",  scriptCMDPartI, fr->name.c_str(), 
		 elapsedTime);
	
	ret = system(scriptCMD);
	if(ret)
	{
	    cerr << "Couldn't invoke user-defined script from straggler catcher " << endl;
	    exit(-1);
	}
	cout<<scriptCMD;
    }

/*
    cout << "Caught straggler: " << fr->name << ", thread: " << threadid 
	 << ", threshold: " << fr->latencyThreshold 
	 << ", elapsed: " << elapsedTime << ", tle " 
	 << fr->thrFuncRecords[threadid].timeAtLastEntry << endl;
*/
}


/* See if we caught a straggler 
 * This can race. We are trying to catch the straggler from two places:
 * application thread and the straggler catcher thread. So an application
 * thread may reset the function entry time while the catcher thread is
 * checking whether there is a strggler. We don't want to synchronize here 
 * to avoid overhead. Just check for the race condition.
 */
inline BOOL catchStraggler(FuncRecord *fr, THREADID threadid)
{
    timespec timeAfter;
    UINT64 elapsedTime, timeNow;
    BOOL caught = FALSE;

    if(fr->thrFuncRecords[threadid].timeAtLastEntry == 0)
	return FALSE;

    clock_gettime(CLOCK_REALTIME, &timeAfter);
    timeNow = (timeAfter.tv_sec * BILLION + timeAfter.tv_nsec);
    elapsedTime = timeNow -
	fr->thrFuncRecords[threadid].timeAtLastEntry;

    /* Checking for a possible race condition, if someone
     * else has reset the timeAtLastEntry since we last
     * checked it. 
     */
    if(elapsedTime == timeNow)
	return FALSE;


    if(elapsedTime > fr->latencyThreshold)
    {
	stragglerCaught(fr, threadid, elapsedTime);
	caught = TRUE;
    }
    return caught;
}

VOID callBefore(FuncRecord *fr)
{
    timespec ts;
    THREADID threadid = PIN_ThreadId();

    assert(fr->thrFuncRecords[threadid].valid);

    /* No error checking here, because this has to be fast.
     * Hopefully we've done enough error checking earlie, so
     * the chance of bad things happening here is very low. 
     */
    clock_gettime(CLOCK_REALTIME, &ts);
    fr->thrFuncRecords[threadid].timeAtLastEntry = ts.tv_sec * BILLION + ts.tv_nsec;
}

VOID callAfter(FuncRecord *fr)
{

    THREADID threadid = PIN_ThreadId();

    assert(fr->thrFuncRecords[threadid].valid);

    /* No error checking here, because this has to be fast.
     * Hopefully we've done enough error checking earlie, so
     * the chance of bad things happening here is very low. 
     */
    catchStraggler(fr, threadid);

    fr->thrFuncRecords[threadid].invCount++;
    fr->thrFuncRecords[threadid].timeAtLastEntry = 0;

}

/* Should be used by the straggler-catcher thread. 
 * This fuction goes over all function records and checks if
 * there are any stragglers. The timeAtLastEntry may be concurrently
 * modified by worker threads -- we are not protecting it with a lock. 
 * That might result in a few false positives. We are willing to make
 * that sacrifice in order to avoid locking. 
 *
 * The thread does acquire a lock to avoid race conditions where we 
 * are modifying the thread-record structure in the application thread
 * while the straggler-catcher thread is looking at it. 
 */

VOID stragglerCatcherThread(void *arg)
{

    cout << "Straggler catcher thread is beginning..." << endl;

    PIN_GetLock(&lock, PIN_ThreadId());
    largestUnusedThreadID++; // stragger catcher will use a thread id
    PIN_ReleaseLock(&lock);

    while(numAppThreads > 0)
    {
	PIN_GetLock(&lock, PIN_ThreadId());

	for (auto& x: funcMap) {

	    FuncRecord *fr = (FuncRecord *) x.second;
	    UINT i;
	
	    for(i = 0; i < largestUnusedThreadID; i++)
	    {
		ThrLocData *tld = &(fr->thrFuncRecords[i]);
		if(tld->valid)
		{
		    BOOL caught = catchStraggler(fr, i);
		    if(caught)
			cout << "*";
		}
	    }
	} 
	PIN_ReleaseLock(&lock);

	/* Now sleep for a while then try again */
	PIN_Sleep(KnobTimeInterval.Value());
    }

    cout << "Straggler catcher thread is exiting..." << endl;
}

VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    BOOL startCatcher = FALSE;

    PIN_GetLock(&lock, threadid+1);

    /* Thread IDs are monotonically increasing and are not reused
     * if a thread exits. */
    largestUnusedThreadID++;
    markThreadRecValid(threadid);
    numAppThreads++;
    if(numAppThreads == 1)
	startCatcher = TRUE;
    PIN_ReleaseLock(&lock);
    

    if(startCatcher)
    {
	/* Spawn the thread that will try to catch stragglers */
	if(PIN_SpawnInternalThread(stragglerCatcherThread, 0, 0, NULL) ==
	   INVALID_THREADID)
	{
	    cerr << "Straggler catcher thread could not be created..." << endl;
	    exit(1);
	}
    }
}

// This routine is executed every time a thread is destroyed.
VOID ThreadFini(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v)
{
    PIN_GetLock(&lock, threadid+1);
    markThreadRecInvalid(threadid);
    numAppThreads--;
    PIN_ReleaseLock(&lock);
}


/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID Image(IMG img, VOID *v)
{
    /* Go over all the routines we are instrumenting and insert the
     * instrumentation.
     */

    for (FuncName* fn = funcNameList; fn; fn = fn->next)
    { 
	THREADID threadid;
	RTN rtn = RTN_FindByName(img, fn->name.c_str());

	if (RTN_Valid(rtn))
	{
	    cout << "Procedure " << fn->name << " located." << endl;

	    // Allocate a record for this routine
	    FuncRecord * fr = new FuncRecord;

	    fr->name = RTN_Name(rtn);

	    fr->image = StripPath(IMG_Name(SEC_Img(RTN_Sec(rtn))).c_str());
	    fr->address = RTN_Address(rtn);
	    fr->latencyThreshold = fn->threshold;
	    
	    // Allocate space for thread-local data
	    fr->thrFuncRecords = (ThrLocData *)malloc(threadArraySize * sizeof(ThrLocData));
	    if(fr->thrFuncRecords == NULL)
	    {
		cerr << "Could not allocate memory in Image routine. Aborting... " << endl;
		exit(-1);
	    }
	    memset((void*)fr->thrFuncRecords, 0, threadArraySize * sizeof(ThrLocData));

	    /* Set the record valid for the current thread, because it won't have
	     * another chance to do so -- the ThreadStart routine where this is
	     * normally done has already run.
	     */
	    threadid = PIN_ThreadId();
	    assert(threadid != INVALID_THREADID);
	    assert(threadid < threadArraySize);

	    fr->thrFuncRecords[threadid].valid = 1;

	    // Add to the map of routines
	    pair<string, FuncRecord*> record(fr->name, fr);

	    PIN_GetLock(&lock, threadid+1);
	    funcMap.insert(record);
	    PIN_ReleaseLock(&lock);

	    RTN_Open(rtn);

	    // Instrument 
	    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBefore,
			   IARG_PTR, fr, IARG_END);
	    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)callAfter,
			   IARG_PTR, fr, IARG_END);

	    RTN_Close(rtn);
	}
    } 
}
/* ===================================================================== */
/* Build the list of procedures we want to instrument                    */
/* ===================================================================== */

/* We don't call this function unless we have 3 elements in the vector */
FuncName* getFN(vector<string> elems){
    
    double threshold;

    FuncName* fn = new FuncName;

    /* Get function name */
    fn->name = elems[0]; 

    /* Get latency threshold */
    threshold = atof(elems[1].c_str());
    if(threshold == 0.0)
    {
	cout << "Invalid parameter for latency threshold: " << "[" << elems[1] << "]" << endl;
	return NULL;
    }

    /* Get the units for latency threshold, convert to nanoseconds */
    string units = elems[2];
    UINT64 multiplier;

    if(units.compare("s") == 0)
	multiplier = BILLION;
    else if(units.compare("ms") == 0)
	multiplier = MILLION;
    else if(units.compare("us") == 0)
	multiplier = THOUSAND;
    else if(units.compare("ns") == 0)
	multiplier = 1;
    else{
	cout << "Invalid unit specified: " << "[" << elems[2] << "]" << endl;
	return NULL;
    }

    fn->threshold = (UINT64) (threshold * multiplier);
    
    return fn;
}

int buildFuncList(ifstream& f)
{
    while(!f.eof())
    {
	vector<string> elems;
	int i;
	
	/* We are looking for sets of three tokens:
	 * func name, value of threshold, time unit for threshold.
	 * E.g.: 
	 *         my_func 3 s
	 * which means that if my_func takes more than 3 seconds to 
	 * run, we will catch it as a straggler. 
	 */ 
	for(i = 0; !f.eof() && i < 3; i++)
	{
	    string word;
	    f >> word;

	    if(word.length() > 0)
		elems.push_back(word);
	}
	if(elems.size() == 3)
	{
	    FuncName *fn = getFN(elems);
	    if(fn == NULL)
		return -1;

	    fn->next = funcNameList;
	    funcNameList = fn;
	}
	else if(elems.size() != 0)
	{
	    unsigned int k;
	    cerr << "Invalid file format. Offending words are: " << endl;
	    for(k = 0; k < elems.size(); k++)
		cout << "<" << elems[k] << ">" << endl;
	    return -1; 
	}
	
    }

    return 0;

}


/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
   
INT32 Usage()
{
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    cerr << "This tool catches functions that exceed their user-defined latency threshold." 
	 << endl;
    cerr << "We expect an input file containing sets of 3 words in the form: " << endl;
    cerr << "<func_name> <value> <unit>" << endl;
    cerr << "For example: "<< endl;
    cerr << "my_func 3 ns" << endl;
    cerr << endl;
    cerr << "In this case we will catch my_func() as a straggler if it runs for "
	 << " more than 3 nanoseconds." << endl;
    cerr << "Valid units are: s, ms, us, ns." << endl;

    return -1;
}

#define MAX_PID_DIGITS 8

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
    
    inputFile.open(KnobInputFile.Value().c_str());
    if(buildFuncList(inputFile))
	return Usage();

    if(strlen(KnobScriptPath.Value().c_str()) > 0)
    {
	scriptProvided = TRUE;

	int cmdLen = strlen(KnobScriptPath.Value().c_str()) + 2 + MAX_PID_DIGITS;
	scriptCMDPartI = (char*)malloc(cmdLen);
	if(scriptCMDPartI == NULL)
	{
	    cerr << "Couldn't malloc " << endl;
	    exit(-1);
	}
	snprintf(scriptCMDPartI, cmdLen, "%s %d", KnobScriptPath.Value().c_str(), getpid());
    }

    /* Register Image to be called to instrument functions.*/
    IMG_AddInstrumentFunction(Image, 0);

    /* Register Analysis routines to be called when a thread begins/ends */
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
    
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
