/*
 * Copyright 2012 by Christopher Hallinan; all rights reserved.
 *         
 * This file may be used subject to the terms and conditions of the
 * GNU General Public License Version 2, as published by the Free
 * Software Foundation.  This program is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A 
 * PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#define MAXPIDS 32768

FILE *outfd;

struct timeval timenow;
int process_count;

/* Fast buffer for directory scanning, indexed by pid */
typedef struct mydirentry_s {
	struct dirent direntry1;
	int cmdline_length;
	struct timeval timestamp;
	int reported;
	} mydirentry_t;

mydirentry_t mydentry[MAXPIDS];

/* This linked list will hold all newly discovered processes by pid */
typedef struct active_pids_s {
	int pid;
	/* char cmdline[4096]; */
	struct active_pids_s *prev;
	struct active_pids_s *next;
	} active_pids_t;

active_pids_t *active_list_head;

long int timeval_subtract(struct timeval *t2, struct timeval *t1)
{
    long int diff = (t2->tv_usec + 1000000 * t2->tv_sec) - (t1->tv_usec + 1000000 * t1->tv_sec);
    return (diff);
}

static int diag_count = 0;
void active_list_add(int pid) {

	/* Add a new item to end of list */
	active_pids_t *ptmp, *pprev;

	active_pids_t *ap = malloc(sizeof(active_pids_t));
	if ( ap == NULL ) {
		fprintf(stderr, "malloc failed - bailing\n");
		exit(2);
	}

	memset(ap, 0, sizeof(active_pids_t));

	/* Store the pid in this list element */
	ap->pid = pid;

	if (active_list_head ) {
		/* We've already got items on the list */
		ptmp = active_list_head;
		/* Scan forward to end of list */
		while ( ptmp->next != NULL ) {
			pprev = ptmp;
			ptmp = ptmp->next;
		}
		ptmp->next = ap;
		ap->prev = ptmp;	/* Link to previous item */
	}
	else {
		/* This would be the first item on the list */
		active_list_head = ap;
	}
	diag_count++;
}

int active_list_delete(int pid) {
	active_pids_t *ptmp, *pprev;
	
	/* Scan to end of list looking for pid */
	if ( active_list_head == NULL ) {
		fprintf(stderr, "active_list_delete: Error list is empty (active_list_head == NULL\n");
	}

	ptmp = active_list_head;
	pprev = NULL;
	while ( ptmp != NULL ) {
		diag_count++;
		if ( ptmp->pid == pid ) {
			/* Found the entry we're looking for, now remove it */
			mydentry[ptmp->pid].direntry1.d_ino = 0;
			mydentry[ptmp->pid].reported = 0;
			if (  pprev == NULL ) {
				/* Special case, first item on list */
				if ( ptmp->next == NULL ) {
					/* this case - only a single item at top of list */
					// fprintf(stderr, "R1:%d\n", ptmp->pid);
					/* Delete the entry from the array */
					free(ptmp);
					active_list_head = NULL;
					return 0;
				}
			}
			else {
				/* This is an item in the middle or possibly the end */
				if ( ptmp->next == NULL ) {
					/* This is the last entry in the list */
					pprev->next = NULL;
					// fprintf(stderr, "R3:%d\n", ptmp->pid);
					free(ptmp);
					return 0;
				}
				else {
					/* This is an entry in the middle */
					active_pids_t *pnext = ptmp->next;
					pprev->next = ptmp->next;	/* Bridge the link */
					pnext->prev = pprev;
					// fprintf(stderr, "R2:%d\n", ptmp->pid);
					free(ptmp);
					return 0;
				}
			}
		}
		pprev = ptmp;
		ptmp = ptmp->next;
	}
	/* If we fell through, we didn't find it */
	fprintf(stderr, "Error removing item from active list (%d)\n", pid);
	return 1;
}

int count_direntries() {
	int i, c = 0;
	struct dirent *pdirent;

	for ( i=0; i<MAXPIDS; i++) {
		pdirent = &mydentry[i].direntry1;
		if ( pdirent->d_ino ) {
			c++;
		}
	}
	return c;
}

void load_dir_entries(DIR *dp) {
	struct dirent *p;
	int i=0, pid;

	rewinddir(dp);
	while ( (p = readdir(dp) ) ) { 
		/* Discard all but /proc/[0-9]* entries */
		if ( isdigit(*p->d_name) == 0 ) {
			continue;
		}       
		pid = atoi(p->d_name);
		mydentry[pid].direntry1 = *p;
		i++;    
	}
}

char *get_cmdline(int pid) {
	int fd, rc, i;
	char cmdline_filename[32];
	static char cbuf[8192];

	snprintf(cmdline_filename, sizeof(cmdline_filename), "/proc/%u/cmdline", pid);

	fd = open(cmdline_filename, O_RDONLY);
	if ( fd == -1 ) {
		/* This process has already terminated */
		return(NULL);
	}

	rc = read(fd, cbuf, sizeof(cbuf));
	if ( rc == -1 ) {
		/* This process no longer exists */
		close(fd);
		return(NULL);
	}

	if ( rc - strlen(cbuf) > 1) {
		/* We need to massage this buffer, it's a series of null-terminated strings */
		for ( i=0; i<rc; i++)
			if ( cbuf[i] == '\0' )
				cbuf[i] = ' ';
		cbuf[rc] = '\0';
	}

	close(fd);
	return cbuf;
}

void print_process(int pid, int timeout) {
	char *pbuf;

	pbuf = get_cmdline(pid);
	if ( pbuf ) {
		mydentry[pid].cmdline_length = strlen(pbuf);
		if ( timeout )
			fprintf(outfd, "%d: %s (T)\n", pid, pbuf);
		else
			fprintf(outfd, "%d: %s\n", pid, pbuf);
	}
	fflush(outfd);
}

int delete_these_pids[1024];
int delete_index;

int check_active_pids(void) {
	/* If we have any active pids, wait about 300 ms and display the cmdline */
	active_pids_t *ptmp;
	char *pcmd;
	int len, active_pid_count;
	struct timeval timenow;
	long int time_diff;

	if ( active_list_head == NULL )
		return 0;

	active_pid_count = 0;
	delete_index = 0;
	ptmp = active_list_head;

	while ( ptmp ) {
		/* We have at least one new active pid on the list */
		active_pid_count++;
		/* See if the length is different from the original report when we detected this new process */
		pcmd = get_cmdline(ptmp->pid);
			if ( pcmd == NULL ) {
				/* This process has apparently terminated */
				delete_these_pids[delete_index++] = ptmp->pid;
				ptmp = ptmp->next;
				continue;
			}

		/* Report the process (display to console) if 1) it's command line changes length or 2) it's been around for > 300ms */
		len = strlen(pcmd);
		if ( len > mydentry[ptmp->pid].cmdline_length ) {
			mydentry[ptmp->pid].cmdline_length = len;
			print_process(ptmp->pid, 0);
		}

		if ( mydentry[ptmp->pid].reported == 0 ) {
			gettimeofday(&timenow, NULL);
			time_diff = timeval_subtract(&timenow, &mydentry[ptmp->pid].timestamp);
			if ( time_diff > 300000 ) {
				print_process(ptmp->pid, 1);
				mydentry[ptmp->pid].reported = 1;
			}
		}

		/* Move on to the next entry in the active pids list */
		ptmp = ptmp->next;
	}

	/* Be sure to remove the entry if it's no longer active */
	for ( delete_index = delete_index - 1; delete_index > 0; delete_index--) {
		active_list_delete(delete_these_pids[delete_index]);
	}
	return active_pid_count;
}

int main(int argc, char **argv) {
	DIR *dp;
	int count, pid;
	struct dirent *p;
	char *pbuf;
	char logfile[128];

	/* Check command line */
	if ( argc > 1 ) {
		if ( strncmp(argv[1], "-o", 2) || ( argc != 3 ) ) {
			fprintf(stderr, "Bad command line: use \"pscanner -o <filename>\"\n");
			exit(1);
		}
		strncpy(logfile, argv[2], 127); 
		printf("Request to print to file: %s\n", logfile); 
		outfd = fopen(logfile, "w");
		if ( outfd == NULL ) {
			perror("fopen failed:");
			exit(1);
		}
	}
	else {
		outfd = stdout;
	}

	active_list_head = NULL;
	process_count = 0;

	memset(mydentry, 0, sizeof(mydentry));

	dp = opendir("/proc");
	if ( dp == NULL ) {
		perror("Error opening /proc\n");
		exit(1);
	}

	/* Prime the pipe by taking a snapshot*/
	load_dir_entries(dp);
	count = count_direntries();
	fprintf(outfd, "Found %d processes\n", count); 

	/* Now enter a loop, scanning the directory for any change */
	while ( 1 ) {
		rewinddir(dp);
		process_count = check_active_pids();

		while ( (p = readdir(dp) ) ) { 
			/* Discard all but /proc/[0-9]* entries */
			if ( isdigit(*p->d_name) == 0 ) {
				continue;
			}
			pid = atoi(p->d_name);
			if ( mydentry[pid].direntry1.d_ino ) {
				/* We know about this PID already, skip it */
				continue;
			}

			/* We have a new process */
			gettimeofday(&timenow, NULL);
			mydentry[pid].timestamp = timenow;

			pbuf = get_cmdline(pid);
			if ( pbuf ) {
				mydentry[pid].cmdline_length = strlen(pbuf);
				active_list_add(pid);
			}

			/* Now refresh our snapshot */
			load_dir_entries(dp);
		}
	}
	fclose(outfd);
	return 0;
}
