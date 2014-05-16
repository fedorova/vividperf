/*
 * What we do. 
 *
 * To parse perf data file a lot of code was borrowed from the Linux source tree
 * tools/perf.
 * 
 * License information goes here. 
 *
 * We have to be very careful to keep the data structures consistent, otherwise we'll
 * be assuming a different file format than what it actually is. You have to assume the 
 * same data structures as in the kernel where the perf file was generated. That's why we
 * don't include uapi/linux/perf_event.h file directly (although it's available at user land), 
 * but copy the data structures into the linux-deps.h. Otherwise, if you have outdated kernel header
 * files installed, you may assume wrong data structure sizes and, therefore, parse the 
 * file incorrectly. 
 *
 * Timestamps. We have to match the timestamps obtained in a user program to those used by 
 * perf, so we can delimit the region of interest. So at user level we have to obtain timestamps
 * in exactly the same manner as perf does it. This is quite challenging, and at the time of
 * the writing cannot be done without changing the kernel. The latter can be done quite easily, 
 * but isn't practical in our context, because not all users of this tool would want to or have
 * the permission to change the kernel. 
 *
 * Perf obtains the timestamp by calling the function local_clock(), which in turn calls sched_clock()
 * on systems with a 'stable' clock. sched_clock() is overridden to call paravirt_sched_clock() on x86 
 * architectures, which translates into native_sched_clock() (see paravirt.c in the Linux kernel).
 * native_sched_clock() reads the tsc register and converts the value using __cycles_2_ns() 
 * (see the implementation of native_sched_clock() in tsc.c). This function relies on dynamically
 * updated per-processor variables, which cannot be obtained at user level. There is discussion
 * among Linux developers to expose perf timer to userland, but the consensus as to what the best solution
 * is hasn't been reached. See this thread: https://groups.google.com/forum/#!topic/fa.linux.kernel/iBmmkY7HWKo
 *
 * Ideally, we'd want to generate user events for perf to delineate the region of interest, but at the time of the 
 * writing perf doesn't support user events as well as the use tracepoint and other hardware/software events 
 * simultaneously. A user event would be a tracepoint event, so inability to track other parameters makes
 * this impractical.
 *
 * To work around this limitation, we use a solution where the user program obtains timestamps using clock_gettime()
 * and then we calibrate them with perf timestamps. When the program is exec'ed and then the executable is renamed, 
 * perf generates a COMM event. This event roughly corresponds to the time when the program begins running. 
 * We ask the user to provide us the timestamps obtained using clock_gettime and CLOCK_MONOTONIC_RAW as early as
 * possible when the program launches (i.e., the first line in "main()") as well as the two timestamps delineating
 * the region of interest obtained with the same clock. We calibrate these timestamps with perf timestamps to find
 * the region of interest. 
 *
 * The described method should work for CPU-intensive programs. For programs that spend significant time idle there
 * might be a discrepancy between perf clock and CLOCK_MONOTONIC_RAW if the machine uses frequency scaling while the
 * CPU is idle. Our hope is that Linux developers provide the solution to this problem at some point, because the
 * current work-around is not ideal.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#include "linux-deps.h"
#include "list.h"

union u64 {
    u64 val64;
    u32 val32[2];
};

/* We keep track of event attributes, because as we parse samples
 * we need to know the format of the samples. Event attributes
 * will give us this information. 
 */
Node *event_attr_list = NULL;

/* To synchronize user-supplied timestamps with perf timestamps, we use the
 * first valid timestamp corresponding to the COMM event to mark the beginning
 * of the execution. A COMM event occurs when the process execs and then when 
 * the executable is renamed. At the time of the writing, only the second COMM
 * event is accompanied by a valid timestamp. 
 *
 * The user also provides a base timestamp corresponding to when his program 
 * began running. Then, we use the begin and end timestamps provided by the user
 * to delimit the time period of interest relative to the base timestamp. 
 * To account by the 'drift' between user and perf timestamps we extend the 
 * period of interest by about 1ms. This is way too crude for very small functions. 
 * but given the limitations of perf timer (see above) this is the best we can
 * do at the moment. 
 */
u64 perf_base_time = 0;
u64 user_base_time = 0;
#define DRIFT 1000000 /* One millisecond based on experimental results */

/* These initialization parameters will ensure that 
 * by default we process events with all timestamps */
u64 begin_time = 0;
u64 end_time = ~0 - DRIFT;


/* 
 * This structure contains the event attribute
 * given to us in the file, plus some more information 
 * that we keep around and use during the parsing. 
 */
typedef struct event_descr {
    struct perf_event_attr attr;
    u64 sample_size;
} event_descr;


/*
 * Helper functions that read/write to a file, check for error and
 * exit if we couldn't read/write as much as we expect. Keeps the
 * rest of the code cleaner. Same for malloc
 */
void
read_and_exit_on_error(int fd, void *buf, size_t size, char *file, int line)
{
    int ret = read(fd, buf, size);
    if(ret < size)
    {
	fprintf(stderr, "Error reading %ld bytes: %s\n", size, strerror(errno));
	fprintf(stderr, "Call made from file: %s, line: %d.\n", file, line);
	exit(-1);
    }
}

void
write_and_exit_on_error(int fd, void *buf, size_t size, char *file, int line)
{
    int ret = write(fd, buf, size);
    if(ret < size)
    {
	fprintf(stderr, "Error writing %ld bytes: %s\n", size, strerror(errno));
	fprintf(stderr, "Call made from file: %s, line: %d.\n", file, line);
	exit(-1);
    }
}

void *
malloc_and_exit_on_error(size_t size, char *file, int line)
{
    void *buffer = malloc(size);
    if(buffer == NULL)
    {
	fprintf(stderr, "Couldn't malloc %ld bytes: %s\n", size, strerror(errno));
	fprintf(stderr, "Call to  malloc made from file: %s, line: %d\n", file, line);
	exit(-1);
    }    
    return buffer;
}

/* 
 * Given the information in the sample_type mask, determine
 * the size of the corresponding sample. This helps us with
 * parsing.  We are only determining the size of the static
 * component. If there are things like callchains and raw samples, there
 * will be a dynamic component to the sample size, which we can't know
 * in advance. 
 *
 * Code borrowed from tools/perf/util/evsel.c
 */
u64 compute_sample_size(u64 sample_type)
{
    u64 mask = sample_type & PERF_SAMPLE_MASK;
    u64 size = 0;
    int i;

    for (i = 0; i < 64; i++) 
    {
	if (mask & (1ULL << i))
	    size++;
    }

    size *= sizeof(u64);

    return size;
}


static char* update_sample_name(char *oldstring, char *name)
{
    char *newstring;
    size_t newstring_size = strlen(oldstring) + strlen(name) + 3;

    newstring = 
	(char *)malloc_and_exit_on_error(newstring_size, __FILE__, __LINE__);
    
    snprintf(newstring, newstring_size, "%s %s", oldstring, name);
    free(oldstring);

    return newstring;
}

/* 
 * Given a sample type, go over the mask and create
 * a string describing what we are tracking. 
 */
char * what_are_we_sampling(u64 type)
{
    char *string;

    string = (char *)malloc_and_exit_on_error(1, __FILE__, __LINE__);
    string[0] = '\0';


    if(type & PERF_SAMPLE_IP)
	string = update_sample_name(string, "PERF_SAMPLE_IP");
    if(type & PERF_SAMPLE_TID)
	string = update_sample_name(string, "PERF_SAMPLE_TID");
    if(type & PERF_SAMPLE_TIME)
	string = update_sample_name(string, "PERF_SAMPLE_TIME");
    if(type & PERF_SAMPLE_ADDR)
	string = update_sample_name(string, "PERF_SAMPLE_ADDR");
    if(type & PERF_SAMPLE_ID)
	string = update_sample_name(string, "PERF_SAMPLE_ID");
    if(type & PERF_SAMPLE_STREAM_ID)
	string = update_sample_name(string, "PERF_SAMPLE_STREAM_ID");
    if(type & PERF_SAMPLE_CPU)
	string = update_sample_name(string, "PERF_SAMPLE_CPU");
    if(type & PERF_SAMPLE_PERIOD)
	string = update_sample_name(string, "PERF_SAMPLE_PERIOD");
    if(type & PERF_SAMPLE_READ)
	string = update_sample_name(string, "PERF_SAMPLE_READ");
    if(type & PERF_SAMPLE_CALLCHAIN)
	string = update_sample_name(string, "PERF_SAMPLE_CALLCHAIN");
    if(type & PERF_SAMPLE_RAW)
	string = update_sample_name(string, "PERF_SAMPLE_RAW");
    if(type & PERF_SAMPLE_BRANCH_STACK)
	string = update_sample_name(string, "PERF_SAMPLE_BRANCH_STACK");
    if(type & PERF_SAMPLE_REGS_USER)
	string = update_sample_name(string, "PERF_SAMPLE_REGS_USER");
    if(type & PERF_SAMPLE_STACK_USER)
	string = update_sample_name(string, "PERF_SAMPLE_REGS_USER");


    return string;
}



/*
 * Read and skip the header. Return 0 if valid header was found and skipped. 
 */
int 
check_and_copy_header(int fd, int ofd, perf_file_header * header)
{
    int ret;

    read_and_exit_on_error(fd, header, sizeof(perf_file_header), __FILE__, __LINE__);

    printf("read %ld bytes of header\n", sizeof(perf_file_header));

    /* Check if the magic number is valid */
    if(!is_perf_magic(header->magic))
    {
	fprintf(stderr, "Invalid file format. Magic number does not pass check.");
	return -1;
    }

    /*
     * the magic number serves two purposes:
     * - unique number to identify actual perf.data files
     * - encode endianness of file
     * We don't bother supporting the opposite endianness for now,
     * assuming that the file is generated and processed
     * on the platform with the same endianness.
     */
    if(header->magic == __perf_magic2_sw)
    {
	fprintf(stderr, "Looks like file endianness doesn't match "
		"the current platform. We don't support that for now.\n");
	return -1;
    }

    /* Old format */
    if(!memcmp(&(header->magic), __perf_magic1, sizeof(header->magic)))
    {
	fprintf(stderr, "Input file is in PERF1 format, which we don't support.");
	return -1;	    
    }

    /* Check the header format. We won't bother supporting the previous
     * version for now. 
     */
    if (header->size != sizeof(*header)) 
    {
	if (header->size == offsetof(typeof(*header), adds_features))
	{
	    fprintf(stderr, "Input file is in the previous format, which we don't support.");
	    return -1;	    
	}
	
    }

    /* Now copy the header to the output file, which contains the manicured data */
    write_and_exit_on_error(ofd, header, sizeof(*header), __FILE__, __LINE__);
    
    return 0;
}

/* 
 * Check if the timestamp of this event falls between begin_time and end_time.
 */
bool event_do_we_care(union perf_event *event, u64 begin_time, u64 end_time)
{

    union u64 u;

    /* With this assignment, we interpret the event data structure as an array 
     * of 64-bit ints. How we interpret this array depends on the event and 
     * sample type.
     */ 
    const u64 *array = event->sample.array;

    /* Here we are going to keep the data that we parsed from the event sample */
    struct perf_sample sample;
    memset(&sample, 0, sizeof(struct perf_sample));
    sample.cpu = sample.pid = sample.tid = -1;
    sample.stream_id = sample.id = sample.time = -1ULL;


    printf("Processed event %s, size %d\n",
	   event->header.type < PERF_RECORD_HEADER_MAX ? perf_event__names[event->header.type]: "UNKNOWN",
	   event->header.size);

    /* 
     * PERF_RECORD_FINISHED_ROUND is a pseudo-event used by perf for convenience.
     * It's not an actual event, but rather a marker in the event trace. See more
     * on this here:
     * https://android.googlesource.com/kernel/omap/+/984028075794c00cbf4fb1e94bb6233e8be08875%5E!/
     *
     * We keep this event, in case our trace will be re-processed by perf tools. 
     */
    if(event->header.type == PERF_RECORD_FINISHED_ROUND)
	return true;

    /* We need to associate perf_event_attr type to this sample. 
     * The way perf does this at the time of the writing (i.e., 3.8.0-38 tools version)
     * is to assume that the first attribute type is the sample type for all the samples
     * we are going to receive. (See tools/perf/util/evlist.c.) 
     */
    event_descr *first_event_descr = (event_descr *) event_attr_list->data;
    struct perf_event_attr *first_attr = &(first_event_descr->attr);
    u64 sample_type = first_attr->sample_type;


    /* The goal of the code below is to determine where in the event record the timestamp lives. 
     * If this is a SAMPLE type event, the timestamp will live in the data array, we just need
     * to figure out where. This information is encoded in the sample type. 
     */    
    if(event->header.type != PERF_RECORD_SAMPLE)
    {
	/* If this is a non-SAMPLE type event, the timestamp will live in the event ID data
	 * located at the end of the payload if sample_id_all is set in the attribute type. 
	 * If sample_id_all is not set, there is no timestamp, so we do not know whether
	 * this event falls within the time range we care about. So we keep it. 
	 */
	if(!first_attr->sample_id_all)
	{
	    fprintf(stderr, "Error: we assume that all events provide sample_id_all. "
		    "Check should have been made before we began event processing. "
		    "Can't continue.\n");
	    exit(-1);
	}
	
	/* 
	 * The ID data should be at the end of the payload. The meaning of that data
	 * depends on what in the sample_type. 
	 * We treat the event as an array of 64-bit ints by assuming a sample_event
	 * structure (see initialization of array above) and step through it backwards. 
	 * The code below is borrowed from perf_evsel__parse_id_sample()
	 * tools/perf/util/evsel.c, line 827 (3.8 kernel).
	 */
	array += ((event->header.size -
		   sizeof(event->header)) / sizeof(u64)) - 1;
	
	/* Skip the information that we don't need */
	if (sample_type & PERF_SAMPLE_CPU) 
	{
	    u.val64 = *array;
	    sample.cpu = u.val32[0];
	    array--;
	}
	if (sample_type & PERF_SAMPLE_STREAM_ID)
	{
	    sample.stream_id = *array;
	    array--;
	}
	if (sample_type & PERF_SAMPLE_ID)
	{
	    sample.id = *array;
	    array--;
	}
	if (sample_type & PERF_SAMPLE_TIME)
	{
	    sample.time = *array;
	    array--;
	}
	if (sample_type & PERF_SAMPLE_TID)
	{
	    u.val64 = *array;
	    sample.pid = u.val32[0];
	    sample.tid = u.val32[1];
	}
    }
    else 	/* This is an event of type PERF_RECORD_SAMPLE */ 
    {

	if(event->header.size != sizeof(event->header) + first_event_descr->sample_size)
	{
	    fprintf(stdout, "This event has a size (%d) that is not the same "
		    "as that expected from the first event attribute (%" PRIu64 ")\n", 
		    event->header.size, sizeof(event->header) + first_event_descr->sample_size);
	    
	    if(event->header.size < sizeof(event->header) + first_event_descr->sample_size)
	    {
		fprintf(stdout, "This event has a size (%d) that is smaller than "
			"that expected from the first event attribute (%" PRIu64 ")\n", 
			event->header.size, sizeof(event->header) + first_event_descr->sample_size);
		exit(-1);
	    }
	}

	if (sample_type & PERF_SAMPLE_IP) 
	{
	    sample.ip = event->ip.ip;
	    array++;
	}
	if (sample_type & PERF_SAMPLE_TID) 
	{
	    u.val64 = *array;

	    sample.pid = u.val32[0];
	    sample.tid = u.val32[1];
	    array++;
	}
	if (sample_type & PERF_SAMPLE_TIME) 
	{
	    sample.time = *array;
	    array++;
	}

	sample.addr = 0;
	if (sample_type & PERF_SAMPLE_ADDR) {
	    sample.addr = *array;
	    array++;
	}

	sample.id = -1ULL;
	if (sample_type & PERF_SAMPLE_ID) {
	    sample.id = *array;
	    array++;
	}

	if (sample_type & PERF_SAMPLE_STREAM_ID) 
	{
	    sample.stream_id = *array;
	    array++;
	}
	if (sample_type & PERF_SAMPLE_CPU) 
	{
	    u.val64 = *array;
	    sample.cpu = u.val32[0];
	    array++;
	}
	/* There's more things that might be available in the sample,
	 * but we don't care about them for now. */

    }
    s64 rel_time = perf_base_time != 0 ? (sample.time - perf_base_time) : -1;

    printf("CPU: %" PRId32 ",\n" 
	   "STREAM_ID: %" PRId64 ",\n" 
	   "SAMPLE_ID: %" PRId64 ",\n" 
	   "TIME: %" PRId64 ",\n" 
	   "PID: %" PRId32 ",\n" 
	   "TID: %" PRId32 ",\n"
	   "RELATIVE TIME: %" PRId64 ",\n", 
	   sample.cpu, sample.stream_id, sample.id, sample.time, 
	   sample.pid, sample.tid, rel_time);

    /* 
     * We use the first non-zero timestamp for the COMM event as the "absolute zero"
     * timestamp. We will compare user-provided timestamps relative to this one. 
     * COMM event happens when the process execs and also when it is renamed after
     * the exec has occurred. It appears that the rename-corresponding COMM event actually generates a 
     * timestamp, while the exec-corresponding COMM event doesn't. 
     * So we use the first COMM event with the non-zero timestamp to mark the start of the program. 
     */
    if(perf_base_time == 0)
	if(event->header.type == PERF_RECORD_COMM &&
	   sample.time > 0)
	    perf_base_time = sample.time;

    if(sample.time <= 0)
    {
	return true;
    }

    if(rel_time > 0 && (rel_time < begin_time || rel_time > (end_time + DRIFT)) )
    {
	printf("SKIPPING... rel_time is %" PRId64 ", begin: %" PRIu64 ", end: %" PRIu64 " \n", 
	       rel_time, begin_time, end_time);
	return false;
    }
    else
	return true;
}

void
usage(char *prog)
{
    printf("%s takes a valid perf.data file generated with linux tools version 3.8 or compatible "
	   "and pipes the data to a new valid perf output file, modifying the data stream to "
	   "include only the data samples between the two time stamps provided as arguments.\n\n", prog);
    printf("Options:\n\n");
    printf("-s <timestamp>  -- Start-of-program timestamp. The timestamp taken as early as possible as soon as the "
	   "program starts running. The timestamps must be obtained using clock_gettime with "
	   "CLOCK_MONOTONIC_RAW or equivalent. "
	   "See code comments to understand how timestamps are used and correlated with perf timestamps.\n");
    printf("Default: 0.\n\n");
    printf("-b <timestamp>  -- Begin timestamp. Records with smaller timestamps are not included in the output file.\n");
    printf("Default: 0.\n\n");
    printf("-e <timestamp>  -- End timestamp. Records with larger timestamps are not included in the output file.\n");
    printf("Default: inf.\n\n");
    printf("-i <file name>  -- Input file name. Default: \"perf.data\".\n\n");
    printf("-o <file name>  -- Output file name. Default: \"perf.data.manicured\".\n\n");

    return;
}

u64 parse_timestamp_and_exit_on_error(char *timestamp, char *which_one)
{
    u64 time;

    char *endptr = timestamp + strlen(timestamp);
    time = (u64)strtoll(timestamp, &endptr, 10);

    if(*timestamp != '\0' && *endptr == '\0')
	return time;
    else
    {
	fprintf(stderr, "You provided an invalid %s time stamp: %s\n", 
		which_one, timestamp);
	exit(-1);
    }
}

/*
 * Arguments:
 * -i  input file. Default: perf.data.
 */
int
main(int argc, char **argv)
{
    int ret, c;
    char *input_fname = "perf.data";
    char *output_fname = "perf.data.manicured";
    char *optval = NULL;

    perf_file_header f_header, f_header_manicured;


    while ((c = getopt (argc, argv, "b:e:i:o:s:")) != -1)
    {
	switch(c)
	{
	case 'b':
	    begin_time = parse_timestamp_and_exit_on_error(optarg, "begin");
	    break;
	case 'e':
	    end_time = parse_timestamp_and_exit_on_error(optarg, "end");
	    break;
	case 'i':
	    input_fname = optarg;
	    break;
	case 'o':
	    output_fname = optarg;
	    break;
	case 's':
	    user_base_time = parse_timestamp_and_exit_on_error(optarg, "start");
	    break;
	case '?':
	default:
	    usage(argv[0]);
	    exit(-1);
	}
    }

    printf("Begin timestamp: %" PRIu64 " \n" 
	   "End timestamp: %" PRIu64 " \n" 
	   "Start (of program) timestamp: %" PRIu64 " \n", 
	   begin_time, end_time, user_base_time);

    if(user_base_time == 0 && (begin_time != 0))
	printf("Warning: zero starting timestamp provided. Your begin and end timestamps will not be correctly "
	       "calibrated to perf timestamps.\n");

    /* Let's reset begin and end timestamps to be relative to the start-of-program timestamp,
     * so we don't have to perform this computation on every event.
     */
    begin_time -= user_base_time;
    end_time -= user_base_time;
    
    if(begin_time < 0 || end_time < 0)
    {
	printf("Your begin or end timestamps are smaller than the starting timestamp. Cannot proceed.\n");
	usage(argv[0]);
	exit(-1);
    }

    int ifd = open(input_fname, O_RDONLY);
    if(ifd == -1)
    {	
	fprintf(stderr, "Could not open %s: %s\n", input_fname, strerror(errno));
	usage(argv[0]);
	exit(-1);		
    }

    int ofd = open(output_fname, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if(ofd == -1)
    {
	fprintf(stderr, "Could not open %s: %s\n", output_fname, strerror(errno));
	usage(argv[0]);
	exit(-1);		
    }


    ret = check_and_copy_header(ifd, ofd, &f_header);
    if(ret)
	exit(-1);		
    printf("Successful header check...\n");

    
    /*
     * Sanity check that perf.data was written cleanly; data size is
     * initialized to 0 and updated only if the on_exit function is run.
     * If data size is still 0 then the file contains only partial
     * information.  Just warn user and process it as much as it can.
     */
    if (f_header.data.size == 0) {
	fprintf(stderr, "WARNING: The %s file's data size field is 0 which is unexpected.\n"
		"Was the 'perf record' command properly terminated?\n",
		input_fname);
    }

    /* Copy the event attribute section */
    {
	
	/* SANITY CHECK.
	 * If these two quantities are not equal, we must
	 * be processing the perf file format that this tool
	 * does not support. 
	 */

	if(f_header.attr_size != sizeof(struct perf_file_attr))
	{
	    fprintf(stderr, "header attr_size (%" PRIu64 ") not equal to "
		    "the size of struct perf_file_attr (%" PRIu64 "). "
		    "Your perf.data file  is the format that this tool "
		    "does not understand. Sorry!\n", 
		    f_header.attr_size, sizeof(struct perf_file_attr));
	    exit(-1);
	}

	/* Now read and copy file sections corresponding to individual attributes. 
	 * We read and copy attributes one by one, because each attribute contains
	 * a pointer to another file location containing more data. And we need to
	 * copy that data as well. 
	 */
	int i, nr_attrs = f_header.attrs.size / f_header.attr_size;
	for(i = 0; i < nr_attrs; i++)
	{
	    perf_file_attr f_attr;
	    struct perf_event_attr *attr = &(f_attr.attr);

	    lseek(ifd, f_header.attrs.offset + i*f_header.attr_size, SEEK_SET);
	    lseek(ofd, f_header.attrs.offset + i*f_header.attr_size, SEEK_SET);

	    read_and_exit_on_error(ifd,  &f_attr, f_header.attr_size, __FILE__, __LINE__); 
	    write_and_exit_on_error(ofd, &f_attr, f_header.attr_size, __FILE__, __LINE__);

	    printf("Set to offset %ld and read %ld bytes of perf_file_attr (%ld size)\n", 
		   f_header.attrs.offset + i*f_header.attr_size, f_header.attr_size, 
		   sizeof(f_attr));
	 	    
	    if(f_attr.ids.size > 0)
	    {
		printf("There's %" PRId64 " bytes of data at offset %" PRId64 "\n", 
		       f_attr.ids.size, 		   
		       f_attr.ids.offset);

		void *buffer = malloc_and_exit_on_error(f_attr.ids.size, __FILE__, __LINE__);
		
		lseek(ifd, f_attr.ids.offset, SEEK_SET);
		lseek(ofd, f_attr.ids.offset, SEEK_SET);

		read_and_exit_on_error(ifd,  buffer, f_attr.ids.size, __FILE__, __LINE__); 
		write_and_exit_on_error(ofd, buffer, f_attr.ids.size, __FILE__, __LINE__); 
	    
		free(buffer);
	    }

	    if(!(attr->sample_type & PERF_SAMPLE_TIME) && !attr->sample_id_all)
	    {
		fprintf(stderr, "Event %s does not sample time. "
			"We do not know how to process such events.\n", 
			attr->type < PERF_TYPE_MAX ? event_attr_names[attr->type]: "UNKNOWN");
		exit(-1);
	    }

	    /* Keep this event attribute in the list. We will later use them to parse samples.
	     * Technically, we need only the first one (see comment in event_do_we_care()), but
	     * let's keep them all just in case. 
	     */
	    event_descr *attr_to_keep = 
		malloc_and_exit_on_error(sizeof(event_descr), __FILE__, __LINE__);

	    attr_to_keep->attr = *attr;	    
	    attr_to_keep->sample_size = compute_sample_size(attr->sample_type);
	    list_insert_and_exit_on_error(&event_attr_list, (void*)attr_to_keep, __FILE__, __LINE__);

	    printf("Found event %s, sample type is %" PRIu64 ", sample size is %" PRIu64 "\n",
		   attr->type < PERF_TYPE_MAX ? event_attr_names[attr->type]: "UNKNOWN",
		   attr->sample_type, attr_to_keep->sample_size);
	    printf("%s\n", what_are_we_sampling(attr->sample_type));

	    if(!attr->sample_id_all)
	    {
		fprintf(stderr, "This perf file does not have sample IDs for all data "
			"(sample_id_all not set on an event attribute)."
			"We rely on sample id timestamp in the COMM event to calibrate "
			"timestamps, so this program won't work without sample id data. "
			"Try using a more recent version of perf. Sorry!\n");
		exit(-1);
	    }
	}
    }
	
    /* Copy the event section */
    {
	void *buffer = malloc_and_exit_on_error(f_header.event_types.size, __FILE__, __LINE__);
	    
	lseek(ifd, f_header.event_types.offset, SEEK_SET);
	lseek(ofd, f_header.event_types.offset, SEEK_SET);
	
	read_and_exit_on_error(ifd,  buffer, f_header.event_types.size, __FILE__, __LINE__);
	write_and_exit_on_error(ofd, buffer, f_header.event_types.size, __FILE__, __LINE__);

	printf("read event_types: %ld bytes at offset %ld\n", f_header.event_types.size, 
	       f_header.event_types.offset);
	
	free(buffer);
    }
    
    /* Cull the data section. 
     * This section is structured as a collection of perf_event records. Each
     * record begins with a header, which has a size field. We don't know in advance how large a 
     * record is or how it is structured.
     * So we are going to read the header first to find out the size and type, and then we read
     * the rest. 
     */
    {
	size_t bytes_processed = 0, bytes_written_to_manicured_file = 0;

	/* We position the input and output file at the start of the data section, 
	 * but from now on, the files may not be moving synchronously if we are
	 * copying only selected records to the output file.
	 */
	lseek(ifd, f_header.data.offset, SEEK_SET);
	lseek(ofd, f_header.data.offset, SEEK_SET);

	/* We now copy records one by one and decide if we care about them. */
	while(bytes_processed < f_header.data.size)
	{
	    union perf_event event;
	    perf_event_header *event_header = (perf_event_header*) &event; 
	    size_t this_event_size;

	    read_and_exit_on_error(ifd, &event, sizeof(perf_event_header), __FILE__, __LINE__);

	    /* Ok, now we know the size of this event record */
	    this_event_size = event_header->size;

	    /* Read the entire event from the file. 
	     * We don't have to re-read the header, because we already have it, 
	     * but let's do it anyway, this makes the code simpler.
	     */
	    lseek(ifd, -sizeof(perf_event_header), SEEK_CUR);
	    read_and_exit_on_error(ifd, &event, this_event_size, __FILE__, __LINE__);

	    /* Ok, now determine if we care about this event. 
	     * Its timestamp must fall between the begin and end timestamps
	     * supplied as arguments. 
	     */
	    if(event_do_we_care(&event, begin_time, end_time))
	    {
		write_and_exit_on_error(ofd, &event, this_event_size, __FILE__, __LINE__);
		bytes_written_to_manicured_file += this_event_size;
	    }	    
	    
	    bytes_processed += this_event_size;
	    printf("IF offset: %ld, OF offset: %ld\n,", lseek(ifd, 0, SEEK_CUR), 
		   lseek(ofd, 0, SEEK_CUR));

	}


	printf("data section: %ld bytes at offset %ld. Processed %ld bytes \n", 
	       f_header.data.size, f_header.data.offset, bytes_processed);

	/* Now let's re-write the file header section of the output file
	 * to update the data section size.
	 */
	f_header_manicured = f_header;
	f_header_manicured.data.size = bytes_written_to_manicured_file;

	lseek(ofd, 0, SEEK_SET);
	write_and_exit_on_error(ofd, &f_header_manicured, sizeof(perf_file_header), __FILE__, __LINE__);

    }

    /* Copy the additional features section.
     * This section begins at the end of the data section and has a number of 
     * records of type perf_file_section. How many records there are is determined
     * by the number of set bits in the adds_features bitmap in the file header. 
     * 
     * If we have not copied the entire data section from the input file to the
     * output file, the output file will have a hole in it. That's ok. 
     * 
     * These additional features (defined in tools/perf/util/header.h) include
     * things like hostname, OS release, NUMA topology, etc. So we copy them
     * unchanged into the manicured file. 
     */
    {
	size_t feat_offset = f_header.data.offset + f_header.data.size;
	int i, nr_records; 

	nr_records = bitmap_weight(f_header.adds_features, HEADER_FEAT_BITS);
	
	/* Each record in this section is a perf_file_section, so it
	 * has a pointer to another file location with data. So we
	 * process these records one by one and copy the record itself and 
	 * the data to which it points. 
	 */
	for(i = 0; i < nr_records; i++)
	{
	    perf_file_section rec;

	    /* For the output file we use the same offset as the
	     * one in the original input file, even though the 
	     * manicured output file is most likely shorter than
	     * the original (because we skipped data records). 
	     * That is probably okay. Only means that our output file
	     * will have a "hole" in it. 
	     */

	    lseek(ifd, feat_offset + i * sizeof(rec), SEEK_SET);
	    lseek(ofd, feat_offset + i * sizeof(rec), SEEK_SET);

	    read_and_exit_on_error(ifd,  &rec, sizeof(rec), __FILE__, __LINE__);
	    write_and_exit_on_error(ofd, &rec, sizeof(rec), __FILE__, __LINE__);

	    printf("Adds feats: read %ld bytes at offset %ld\n", sizeof(rec), 
		   feat_offset + i * sizeof(rec));

	    printf("There's %ld more bytes at offset %ld\n", rec.size, rec.offset);

	    lseek(ifd, rec.offset, SEEK_SET);
	    lseek(ofd, rec.offset, SEEK_SET);

	    void *buffer = malloc_and_exit_on_error(rec.size, __FILE__, __LINE__);

	    read_and_exit_on_error(ifd,  buffer, rec.size, __FILE__, __LINE__);
	    write_and_exit_on_error(ofd, buffer, rec.size, __FILE__, __LINE__);
	    
	    free(buffer);
	}
	
    }

}
