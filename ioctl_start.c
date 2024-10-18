/* Copyright (c) 2017, 2024 James Bruska, Caleb DeLaBruere, Chutitep Woralert

This file is part of K-LEB.

K-LEB is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

K-LEB is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with K-LEB.  If not, see <https://www.gnu.org/licenses/>. */
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include "kleb.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <signal.h>
#include <string.h>

/* Check interrupt */
static int checkint;
static char logpath[200];

/* Handle interrupt */
void sigintHandler(int sig_num){
	signal(SIGINT, sigintHandler);
	printf("Stop monitoring....\n");
	checkint=1;
}
/* Convert event name to event code */
unsigned int NameToRawConfigMask(char* event_name)
{
	/* Branch Events **/
	if      (strcmp(event_name,"BR_RET") == 0) return BR_RET;
	else if (strcmp(event_name,"BR_MISP_RET") == 0) return BR_MISP_RET;
	/** Cache Events **/
	else if (strcmp(event_name,"LOAD") == 0) return LOAD;
	else if (strcmp(event_name,"STORE") == 0) return STORE;
	else if (strcmp(event_name,"L1_ICACHE_MISS") == 0) return L1_ICACHE_MISS;
	else if (strcmp(event_name,"MISS_LLC") == 0) return MISS_LLC;
	/** Instruction Events **/
	else if (strcmp(event_name,"INST_RET") == 0) return INST_RET;
	else if (strcmp(event_name,"NEAR_RET") == 0) return NEAR_RET;
	else if (strcmp(event_name,"INST_FP") == 0) return INST_FP;
	else if (strcmp(event_name,"ARITH_MULT") == 0) return ARITH_MULT;
	else if (strcmp(event_name,"ARITH_DIV") == 0) return ARITH_DIV;
	/** TLB Events **/
	else if (strcmp(event_name,"L1_MISS_DTLB") == 0) return L1_MISS_DTLB;
	else if (strcmp(event_name,"STLB_HIT") == 0) return STLB_HIT;
	/* UNKNOWN Event */
	else return UNKNOWN_EVENT;
}

int val_extract(unsigned int** hardware_events_buffer, int recording, int event,  FILE* log_path)
{
	int sample_count=0;
	int checkempty = 0;
	int i,j;
	for ( j=0; j < recording && !checkempty; ++j ) {
		for ( i=0; i < (event) && !checkempty; ++i ) { 
			if(hardware_events_buffer[i][j]==-10)
			{
				/* End of data buffer */
				checkempty = 1;
				break;
			}
			else{
				fprintf(log_path, "%d,", hardware_events_buffer[i][j]);
			}
		}
		/* End of Events */
		if(!checkempty){
			++sample_count;
			fprintf(log_path, "\n");
		}
	}
	fflush(log_path);
	return sample_count;
}

kleb_ioctl_args_t parse_cmd(int argc, char *argv[])
{
	int index;
	int num_events = 0;
	int pid;
	float hrtimer = 1;
	char eventname[20];

	kleb_ioctl_args_t kleb_ioctl_args;

	/* Default Timer & Monitoring Mode */
	kleb_ioctl_args.delay_in_ns = 10000000;
	kleb_ioctl_args.user_os_rec = 1; //User:1 OS:2 Both:3

	/* Default logpath */
	strcpy(logpath,"./Output.csv");

	for (index = 1; index < argc; ++index){
		if(argv[index][0] == '-'){
			if(argv[index][1] == 'a'){
				//mode = 1;
				kleb_ioctl_args.pid = 1;
				printf("Set monitor all\n");
			}
			if(argv[index][1] == 't'){
				++index;
				hrtimer = strtof(argv[index], NULL);
				kleb_ioctl_args.delay_in_ns = hrtimer*1000000;
			}
			if(argv[index][1] == 'o'){
				++index;
				strcpy(logpath,argv[index]);				
			}
			if(argv[index][1] == 'm'){
				++index;
				kleb_ioctl_args.user_os_rec = strtol(argv[index], NULL, 10);
			}
			if(argv[index][1] == 'e'){
				++index;
				memset(eventname, 0, sizeof(eventname));
				for(int i = 0; i < strlen(argv[index]); ++i){
					
					if(argv[index][i] == ','){
						if(isalpha(eventname[0])){		
							if(eventname[0] == 'f' && eventname[1] == 'f' || eventname[0] == 'F' && eventname[1] == 'F'){
								kleb_ioctl_args.counter[num_events] = strtol(eventname, NULL, 16);
							}
							else{
								kleb_ioctl_args.counter[num_events] = NameToRawConfigMask(eventname);
							}	
						}
						else{
							kleb_ioctl_args.counter[num_events] = strtol(eventname, NULL, 16);
						}
						//printf("EVT: %u %s %d\n", *kleb_ioctl_args.counter[num_events], eventname, num_events);
						++num_events;
						memset(eventname, 0, sizeof(eventname));
					}
					else{
						strncat(eventname, &argv[index][i], 1);
						if(i == (strlen(argv[index])-1)){					
							if(isalpha(eventname[0])){		
								if(eventname[0] == 'f' && eventname[1] == 'f' || eventname[0] == 'F' && eventname[1] == 'F'){
									kleb_ioctl_args.counter[num_events] = strtol(eventname, NULL, 16);
								}
								else{
									kleb_ioctl_args.counter[num_events] = NameToRawConfigMask(eventname);
								}	
							}
							else{
								kleb_ioctl_args.counter[num_events] = strtol(eventname, NULL, 16);
							}
							//printf("EVT: %u %s %d\n", *kleb_ioctl_args.counter[num_events], eventname, num_events);
							memset(eventname, 0, sizeof(eventname));
							++num_events;
							kleb_ioctl_args.num_events = num_events;
						}
					}
				}
				
			}
		}
		else{
			break;
		}
	}
	    /* Parameters Parser */
	if(kleb_ioctl_args.num_events > 6){
		printf("This module only support monitoring up to 6 events\n");
		exit(0);
	}
	if(kleb_ioctl_args.num_events == 0){
		/* Set default event if none provided */
		kleb_ioctl_args.counter[0] = strtol("00c2", NULL, 16);
		kleb_ioctl_args.counter[1] = strtol("00c3", NULL, 16);
		kleb_ioctl_args.counter[2] = strtol("0129", NULL, 16);
		kleb_ioctl_args.counter[3] = strtol("0229", NULL, 16);
		kleb_ioctl_args.counter[2] = strtol("0729", NULL, 16);
		kleb_ioctl_args.counter[3] = strtol("00c0", NULL, 16);
		num_events = 6;
		kleb_ioctl_args.num_events = num_events;
	}


	if(kleb_ioctl_args.pid != 1)
	{
		kleb_ioctl_args.pid = atoi(argv[index]);
		//printf("%d %d\n",kleb_ioctl_args.pid, kill(kleb_ioctl_args.pid, 0));
		if(!kill(kleb_ioctl_args.pid, 0) && kleb_ioctl_args.pid != 0)
		{
			printf("Monitor PID %d\n", kleb_ioctl_args.pid);
		}
		else
		{
			/* User pass in program path */
			printf("Monitor Program %s\n", argv[index]);
			//usrcmd = 1;
			pid = fork();
			if (pid == 0)
			{ 
				/* Execute child */
				sleep(1);
				/* Phrase program arguments */
				char* cmdargv[argc];
				for (int i = index; i < argc; ++i){
					cmdargv[i-index] = argv[i];
				}
				cmdargv[argc-index]=NULL;
				execvp(argv[index], cmdargv);
				perror("Error: ");
				exit(0);
			}
			else{
				/* Set pid to monitor */
				kleb_ioctl_args.pid = pid;
			}
		}
	}

	return kleb_ioctl_args;
}

void deinit_ioctl(int fd)
{
	if(ioctl(fd, IOCTL_STOP, "Stopping") < 0)
	{
		printf("ioctl failed and returned errno %s \n",strerror(errno));
	}
	printf("Deinitializing K-LEB...\n");
}

void init_log(FILE* logfp, kleb_ioctl_args_t kleb_ioctl_args)
{
	int j;
	printf("Logging data...\n");
	for(j = 0; j < (kleb_ioctl_args.num_events); ++j){
		fprintf(logfp, "%x,", kleb_ioctl_args.counter[j]);
	}
	fprintf(logfp, "\n");
	
	printf("Log Path: %s\n ", logpath);
}

int read_kernel_buffer(int fd, unsigned int **hardware_events, int size_of_message, int num_sample, int num_recordings, kleb_ioctl_args_t kleb_ioctl_args, FILE* logfp)
{
	/* Extract data from kernel */
	int ret = read(fd, hardware_events[0], size_of_message);
	if (ret < 0)
	{
		perror("Failed to read the message from the device.\n");
		deinit_ioctl(fd);
		exit(0);
		return errno;
	}
	else if (ret == 0){
		/* Empty data */
		printf("No Data to Extract %d\n", ret);
	}
	else{
		num_sample += val_extract(hardware_events, num_recordings, kleb_ioctl_args.num_events, logfp);
	}
	return num_sample;
}
void exit_monitoring(int fd, unsigned int **hardware_events, int size_of_message, int num_sample, int num_recordings, kleb_ioctl_args_t kleb_ioctl_args, FILE* logfp){
	deinit_ioctl(fd);
	printf("Sample Exit: %d\n", num_sample);
	num_sample = read_kernel_buffer(fd, hardware_events, size_of_message, num_sample, num_recordings, kleb_ioctl_args, logfp);
	printf("Sample Last Extract: %d\n", num_sample);
	printf("Finish Extract last data... \n");
	printf("Stopping K-LEB...\n# of Sample: %d\n", num_sample);

}
void start_monitoring(int fd, kleb_ioctl_args_t kleb_ioctl_args)
{
	checkint=0;
	signal(SIGINT, sigintHandler);

	int status = 0;
	int i;
	int num_sample = 0;
	int num_recordings = 500;
	struct timespec t1, t2;
	unsigned long long int tap_time;

	/* Set user tapping time */
	tap_time = kleb_ioctl_args.delay_in_ns*60;
	if(tap_time >= 1000000000 ){
		t1.tv_sec = tap_time/1000000000;
		t1.tv_nsec = tap_time-(t1.tv_sec*1000000000);
	}
	else{
		t1.tv_sec = 0;
		t1.tv_nsec = tap_time;
	}

	/* Buffer for tapping */
	unsigned int **hardware_events = malloc( (kleb_ioctl_args.num_events)*sizeof(unsigned int *) );
	hardware_events[0] = malloc( (kleb_ioctl_args.num_events)*num_recordings*sizeof(unsigned int) );
	for ( i=0; i < (kleb_ioctl_args.num_events); ++i ) {
		hardware_events[i] = *hardware_events + num_recordings * i;
	}

	/* Buffer for storing data */
	long int hardware_events_buffer[kleb_ioctl_args.num_events][100000];
	/* Overflow value */
	int size_of_message = num_recordings * (kleb_ioctl_args.num_events) * sizeof(long int);
	/*  Log to file	*/
	FILE *logfp = fopen(logpath, "w");
	if( logfp == NULL ){
		fprintf(stderr,"Error opening file: %s\n", strerror(errno));
		exit(0);
	}
	else{
		init_log(logfp, kleb_ioctl_args);
	}
	
	if(kleb_ioctl_args.pid == 1){		
		/* Monitor system */
		printf("Monitoring HPC... \nPress Ctrl+C to exit\n");
		while (!checkint) {	
			nanosleep(&t1, &t2);
			num_sample = read_kernel_buffer(fd, hardware_events, size_of_message, num_sample, num_recordings, kleb_ioctl_args, logfp);
			//printf("Sample: %d\n", num_sample);
		}
	}
	else{

		printf("%d %d\n",kill(kleb_ioctl_args.pid, 0), waitpid(kleb_ioctl_args.pid, &status, WNOHANG));
		/* Monitor pid */
		if(waitpid(kleb_ioctl_args.pid, &status, WNOHANG) == -1){
			printf("Monitoring HPC... \nWait for pid %d \nPress Ctrl+C to exit\n", kleb_ioctl_args.pid);	
			while (!kill(kleb_ioctl_args.pid, 0) &&  !checkint) {

				nanosleep(&t1, &t2);
				/* Extract data from kernel */
				num_sample = read_kernel_buffer(fd, hardware_events, size_of_message, num_sample, num_recordings, kleb_ioctl_args, logfp);
				//printf("Sample: %d\n", num_sample);
			}
		}
		else
		{
			printf("Monitoring HPC... \nWait for Program %d \nPress Ctrl+C to exit\n", kleb_ioctl_args.pid);	
			while (!waitpid(kleb_ioctl_args.pid, &status, WNOHANG) && !checkint) {
				nanosleep(&t1, &t2);
				/* Extract data from kernel */
				num_sample = read_kernel_buffer(fd, hardware_events, size_of_message, num_sample, num_recordings, kleb_ioctl_args, logfp);
				//printf("Sample: %d\n", num_sample);
			}
		}
	}
	exit_monitoring(fd, hardware_events, size_of_message, num_sample, num_recordings, kleb_ioctl_args, logfp);
	fclose(logfp);
}

void init_ioctl(int fd, kleb_ioctl_args_t kleb_ioctl_args)
{
	if(ioctl(fd, IOCTL_START, &kleb_ioctl_args) < 0)
	{
		printf("ioctl failed and returned errno %s \n",strerror(errno));
		exit(-1);
	}
	printf("Initializing K-LEB...\n");
	start_monitoring(fd, kleb_ioctl_args);
}

int main(int argc, char **argv)
{
	// ARGS: Counter1, Counter2, Counter3, Counter4, Timer Delay (in ms), Log path, Program path
	if(argc < 2){
		printf("Error reading configurations\nExiting...\n");
		exit(0);
	}

	kleb_ioctl_args_t kleb_ioctl_args;

	kleb_ioctl_args = parse_cmd(argc,argv);
	printf("PID: %d Events: %u %u %u %u\n Timer: %u\n Log: %s\n",kleb_ioctl_args.pid ,kleb_ioctl_args.counter[0], kleb_ioctl_args.counter[1], kleb_ioctl_args.counter[2], kleb_ioctl_args.counter[3], kleb_ioctl_args.delay_in_ns/1000000, logpath);

	int fd = open(DEVICE_PATH, O_RDWR);
	if (fd == -1)
	{
		printf("Error in opening file \n");
		perror("Emesg");
		exit(-1);
	}

	init_ioctl(fd, kleb_ioctl_args);

	close(fd);

}
