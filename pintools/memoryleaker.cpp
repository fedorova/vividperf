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

#define LEAK_MEMORY 1

/* ===================================================================== */
/* Analysis routines                                                     */
/* ===================================================================== */

/* 
 * Recording function-begin and function-end delimiters. 
 * Every time the function is called, we ask Pin to 
 * give us its name. I have tried obtaining the name
 * at the time we insert the instrumentation and cache
 * it, but this caused Pin to run out of memory when the
 * tool was run on "real" applications. 
 * So we dynamically obtain the name to avoid running
 * the memory deficit.
 */

VOID callBeforeFunction(VOID *rtnAddr)
{
    PIN_GetLock(&lock, PIN_ThreadId() + 1);

#if LEAK_MEMORY
    {

    string name = RTN_FindNameByAddress((ADDRINT)rtnAddr);
    
    cout << "function-begin1: " << PIN_ThreadId() << " " 
	 << name << endl;
    }
#else

    cout << "function-begin: " << PIN_ThreadId() << " " 
	 << RTN_FindNameByAddress((ADDRINT)rtnAddr) << endl;
#endif

    cout.flush();
    PIN_ReleaseLock(&lock);
}

VOID callAfterFunction(VOID *rtnAddr)
{

    PIN_GetLock(&lock, PIN_ThreadId() + 1);

#if LEAK_MEMORY
    {
    string name = RTN_FindNameByAddress((ADDRINT)rtnAddr);
    
    cout << "function-end1: " << PIN_ThreadId() << " " 
	 << name << endl;
    }
#else

    cout << "function-end: " << PIN_ThreadId() << " " 
	 << RTN_FindNameByAddress((ADDRINT)rtnAddr) << endl;
#endif

    cout.flush();
    PIN_ReleaseLock(&lock);
}



/* ===================================================================== */
/* Instrumentation routines                                              */
/* ===================================================================== */
   
VOID instrumentRoutine(RTN rtn, VOID * unused)
{
    RTN_Open(rtn);
	    

    /* Instrument entry and exit to the routine to report
     * function-begin and function-end events.
     */
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)callBeforeFunction,
		   IARG_PTR, RTN_Address(rtn), IARG_END);
    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)callAfterFunction,
		   IARG_PTR, RTN_Address(rtn), IARG_END);


    RTN_Close(rtn);
}



VOID Fini(INT32 code, VOID *v)
{
    cout << "PR DONE" << endl;
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
        return -1;
    }    
    
    PIN_InitLock(&lock);


    RTN_AddInstrumentFunction(instrumentRoutine, 0);
    PIN_AddFiniFunction(Fini, 0);

    // Never returns
    PIN_StartProgram();
    
    return 0;
    
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
