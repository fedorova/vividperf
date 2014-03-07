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


#include <fstream>
#include <iomanip>
#include <iostream>
#include <string.h>
#include <sys/time.h>
#include "pin.H"

/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
using namespace std;

ofstream traceFile;
ifstream inputFile; 

/* ===================================================================== */
/* Commandline Switches */
/* ===================================================================== */

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "procinstr.out", "specify trace file name");

KNOB<string> KnobInputFile(KNOB_MODE_WRITEONCE, "pintool",
    "i", "procnames.in", "specify filename with procedures to instrument");

/* ===================================================================== */


/* Holds names of routines we want to instrument
 * Because of inlining there can be more than one
 * instance of a routine with the same name, and we
 * will track those instances separately, so we 
 * keep the name list separate from the routine object list. 
 */
typedef struct RtnName
{
    string _name;
    struct RtnName * _next;
} RTN_NAME;
RTN_NAME * RtnNameList = 0;

// Holds routine objects and instrumentation data
typedef struct RtnInfo
{
    string _name;
    string _image;
    ADDRINT _address;
    RTN _rtn;
    UINT64 _invCount;
    UINT64 _rtnCountExit;
    UINT64 _timeOnEntry;
    UINT64 _cumTime;
    struct RtnInfo * _next;
} RTN_INFO;

// Linked list of instruction counts for each routine
RTN_INFO * RtnList = 0;

const char * StripPath(const char * path)
{
    const char * file = strrchr(path,'/');
    if (file)
        return file+1;
    else
        return path;
}

#define BILLION 1000000000

/* ===================================================================== */
/* Analysis routines                                                     */
/* ===================================================================== */
 
VOID callBefore(RTN_INFO *ri)
{
    timespec ts;

    ri->_invCount++;
    clock_gettime(CLOCK_REALTIME, &ts);
    ri->_timeOnEntry = ts.tv_sec * BILLION + ts.tv_nsec;
}

VOID callAfter(RTN_INFO *ri)
{
    timespec timeAfter;
    
    clock_gettime(CLOCK_REALTIME, &timeAfter);
    ri->_cumTime += (timeAfter.tv_sec * BILLION + timeAfter.tv_nsec) -
	ri->_timeOnEntry;

}

/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID Image(IMG img, VOID *v)
{
    /* Go over all the routines we are instrumenting and insert the
     * instrumentation.
     */

    for (RTN_NAME * rn = RtnNameList; rn; rn = rn->_next)
    { 
	RTN rtn = RTN_FindByName(img, rn->_name.c_str());

	if (RTN_Valid(rtn))
	{
	    cout << "Procedure " << rn->_name << " located." << endl;

	    // Allocate a counter for this routine
	    RTN_INFO * ri = new RTN_INFO;

	    // The RTN goes away when the image is unloaded, so save it now
	    // because we need it in the fini
	    ri->_name = RTN_Name(rtn);

	    ri->_image = StripPath(IMG_Name(SEC_Img(RTN_Sec(rtn))).c_str());
	    ri->_address = RTN_Address(rtn);
	    ri->_invCount = 0;

	    // Add to list of routines
	    ri->_next = RtnList;
	    RtnList = ri;

	    RTN_Open(rtn);

	    // Instrument 
	    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBefore,
			   IARG_PTR, ri, IARG_END);
	    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)callAfter,
			   IARG_PTR, ri, IARG_END);

	    RTN_Close(rtn);
	}
    }

}
/* ===================================================================== */
/* Build the list of procedures we want to instrument                    */
/* ===================================================================== */

VOID buildProcedureList(ifstream& f)
{
    string procName;

    while(!f.eof())
    {
	RTN_NAME *rn = new RTN_NAME;
	f >> rn->_name;
	cout << rn->_name << endl;
	rn->_next = RtnNameList;
	RtnNameList = rn;
    }

}


/* ===================================================================== */

VOID Fini(INT32 code, VOID *v)
{

    traceFile << setw(23) << "Procedure" << " "
	      << setw(15) << "Image" << " "
	      << setw(18) << "Address" << " "
	      << setw(12) << "Calls" << " "
	      << setw(12) << "Avg. Cycles" << endl;

    for (RTN_INFO * ri = RtnList; ri; ri = ri->_next)
    {
	if(ri->_invCount>0)
	    traceFile << setw(23) << ri->_name << " "
		      << setw(15) << ri->_image << " "
		      << setw(18) << hex << ri->_address << dec <<" "
		      << setw(12) << ri->_invCount << " "
		      << setw(12) << ri->_cumTime/ri->_invCount << endl;	
	else
	    traceFile << setw(23) << ri->_name << " "
		      << setw(15) << ri->_image << " "
		      << setw(18) << hex << ri->_address << dec <<" "
		      << setw(12) << ri->_invCount << " "
		      << setw(12) << ri->_cumTime << endl;	
    }
    

    traceFile.close();
}

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
    inputFile.open(KnobInputFile.Value().c_str());
    buildProcedureList(inputFile);

    /* Register Image to be called to instrument functions.*/
    IMG_AddInstrumentFunction(Image, 0);
    PIN_AddFiniFunction(Fini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
    
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
