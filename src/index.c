
#include "libmosaik2.h"

static int exiting = 0;
static void sigHandler(int sig);

uint32_t get_max_tiler_processes(uint32_t max_tiler_processes);
double get_max_load_avg(uint32_t max_load_avg);
double read_system_load();
void check_pid_file(mosaik2_database *md);
void write_pid_file(mosaik2_database *md);
void remove_pid_file(mosaik2_database *md);
void process_input_data(mosaik2_arguments *, mosaik2_context *ctx, mosaik2_database *md);
void process_next_line(mosaik2_arguments *, mosaik2_context *ctx, mosaik2_database *md, char *line, ssize_t i,FILE *);
//void signal_handler(int signal);
void mosaik2_index_add_tiler_pid(mosaik2_context *, pid_t);
void mosaik2_index_clean_tiler_pids(mosaik2_context *);
void print_usage(char *);




void /* Examine a wait() status using the W* macros */
printWaitStatus(const char *msg, int status)
{
if (msg != NULL)
printf("%s", msg);
if (WIFEXITED(status)) {
printf("child exited, status=%d\n", WEXITSTATUS(status));
} else if (WIFSIGNALED(status)) {
printf("child killed by signal %d (%s)",
WTERMSIG(status), strsignal(WTERMSIG(status)));
#ifdef WCOREDUMP /* Not in SUSv3, may be absent on some systems */
if (WCOREDUMP(status))
printf(" (core dumped)");
#endif
printf("\n");
} else if (WIFSTOPPED(status)) {
printf("child stopped by signal %d (%s)\n",
WSTOPSIG(status), strsignal(WSTOPSIG(status)));
#ifdef WIFCONTINUED /* SUSv3 has this, but older Linux versions and
some other UNIX implementations don't */
} else if (WIFCONTINUED(status)) {
printf("child continued\n");
#endif
} else { /* Should never happen */
printf("what happened to this child? (status=%x)\n",
(unsigned int) status);
}
}

int mosaik2_index(mosaik2_arguments *args) {

	char *mosaik2_database_name = args->mosaik2db;
	uint32_t max_tiler_processes = args->max_jobs;
	uint32_t max_load_avg = args->max_load;

	//signal(SIGINT, signal_handler);

	mosaik2_context ctx;
	init_mosaik2_context(&ctx);
	ctx.max_tiler_processes = get_max_tiler_processes(max_tiler_processes);
	ctx.max_load_avg = get_max_load_avg(max_load_avg);
//	fprintf(stderr, "max_load_avg %f\n", ctx.max_load_avg, ctx.max_load_avg);

	mosaik2_database md;
	
	init_mosaik2_database(&md, mosaik2_database_name);
	check_thumbs_db(&md);
	mosaik2_database_read_database_id(&md);
	check_pid_file(&md);
	write_pid_file(&md);

	process_input_data(args, &ctx, &md);

	remove_pid_file(&md);

	return 0;
}

uint32_t get_max_tiler_processes(uint32_t max_tiler_processes) {
	if(max_tiler_processes < 1)
		return sysconf(_SC_NPROCESSORS_ONLN);
	if(max_tiler_processes<=MOSAIK2_CONTEXT_MAX_TILER_PROCESSES)
		return max_tiler_processes;
	fprintf(stderr, "max_tiler_processes is limited to %i\n", MOSAIK2_CONTEXT_MAX_TILER_PROCESSES);
	exit(EXIT_FAILURE);
}

double get_max_load_avg(uint32_t max_load_avg) {
	if(max_load_avg<1)
		return sysconf(_SC_NPROCESSORS_ONLN) * 1.1;
	return max_load_avg*1.0;
}

double read_system_load() {
	char buf[BUFSIZ];
	memset(buf,0,BUFSIZ);
	FILE *loadfile = m_fopen("/proc/loadavg","r");

	size_t read = fread(&buf,1,BUFSIZ,loadfile);
	if(read<1) {
		fprintf(stderr, "could not read any data from /proc/loadavg");
		exit(EXIT_FAILURE);
	}
	m_fclose(loadfile);

	char *token0 = strtok(buf, " ");
	if(token0 == NULL ) {
		fprintf(stderr, "could not split /proc/loadavg\n");
		exit(EXIT_FAILURE);
	}

	return atof(token0);
}

void check_pid_file(mosaik2_database *md) {
	if( access(md->pid_filename, F_OK) == 0) {
		fprintf(stderr, "The pid file (%s) exists, either an indexing process is running or the program did not exit correctly. In the first case, the pid file can be deleted.\n", md->pid_filename);
		exit(EXIT_FAILURE);
	}
}

void write_pid_file(mosaik2_database *md) {
	FILE *f = m_fopen(md->pid_filename,"w");
	fprintf(f, "%i", getpid());
	m_fclose( f );
}

void remove_pid_file(mosaik2_database *md) {
	if((remove(md->pid_filename)) < 0) {
		fprintf(stderr, "could not remove active pid file (%s)", md->pid_filename);
		perror("error");
		exit(EXIT_FAILURE);
	}
}


void process_input_data(mosaik2_arguments *args, mosaik2_context *ctx, mosaik2_database *md) {
	
	//mosaik2_indextask task_list[ctx->max_tiler_processes];
	
	md->tilecount = read_thumbs_conf_tilecount(md);
	size_t i=read_thumbs_db_count(md);
	size_t maxmemb=-1;
	size_t len = 0;
	char *lineptr = NULL;

	ssize_t readcount;
	FILE *stdin0 = stdin;
	if(stdin0==NULL) {
		perror("failed open file");
		exit(1);
	}
	ctx->start_t = time(NULL);
	while( (readcount = getline(&lineptr, &len, stdin0)) > 0 && exiting == 0 && i < maxmemb) {
		process_next_line(args, ctx, md, lineptr, i++,stdin0);
	}
	free(lineptr);
	if(exiting == 1) {
		fprintf(stderr, "received SIGINT, exiting after %li lines\n", i);
	}
	if(i>=maxmemb) {
		fprintf(stderr, "exiting after maximum lines (%li) saved per mosaik2 database, append outstanding images to a new mosaik2 database\n", maxmemb);
	}
		
	int wstatus=0;
	wait(&wstatus); //TODO doesnt work always
}

void process_next_line(mosaik2_arguments *args, mosaik2_context *ctx, mosaik2_database *md, char *line, ssize_t i, FILE *file) {

  if(ctx->exiting)
		fprintf(stdout, "input data is not resumed, EXITing because of SIGTERM.");

	mosaik2_indextask task;
//	memset(&task, 0, sizeof(task));
	
	char *token0 = strtok(line, "\t");
	char *token1 = strtok(NULL, "\t");
	char *token2 = strtok(NULL, "\n");
	if(token0 == NULL || token1 == NULL || token2 == NULL ) {
		errx(EINVAL, "could not split line by tabstop into three token (%s,%s,%s) filename is empty. (linenumber:%li, line:[%s])\n",token0,token1,token2,i, line);
	}

	if(strlen(token0)>=sizeof(task.filename)) {
		errx(EINVAL, "ups, this filename is to long. sorry you have to remove or rename anything, that exceeds %li characters\n", sizeof(task.filename));
	}

	strncpy(task.filename, token0, strlen(token0)+1);
	

	task.filesize = atol(token1);
	task.lastmodified = atoll(token2);
	//here to fork

	//fprintf(stdout,"%i cp: %i mp: %i\n", getpid(), ctx->current_tiler_processes, ctx->max_tiler_processes);
	double load = read_system_load();
	//fprintf(stderr, "load:%f max-load:%f\n", load, ctx->max_load_avg);


	while(ctx->current_tiler_processes >= ctx->max_tiler_processes
			// max_load will only reduce in case over "over"load the concurrent_tiler_processes to 1 at a time
			|| ( load >= ctx->max_load_avg && ctx->current_tiler_processes > 0 )) {
		mosaik2_index_clean_tiler_pids(ctx);
	}

	pid_t pid = fork(); // 0.2 ms
	
	// forked child
	if(pid==0) {
		// closing the input file now, because there where reproduceable invalid data in the main process. Don't know why.
		m_fclose(file);
		mosaik2_tiler(args, md, &task);

		if(args->quiet == 0 || i == 0 || i % 1000 == 0) {
			int jobs = ctx->current_tiler_processes + 1;
			double img_per_min = 60.*i/(time(NULL)-ctx->start_t);
			fprintf(stdout, "job #%li, jobs:%i img/min:%.2f load:%.2f\n", i, jobs, img_per_min, load);
			exit(0);
		}
		exit(0);
	} else {
		//parent process 
		mosaik2_index_add_tiler_pid(ctx, pid);
		if(signal(SIGINT, sigHandler) == SIG_ERR)
			fprintf(stderr, "sigint in user func\n");
	}
	return;
}

void mosaik2_index_write_to_disk(mosaik2_database *md, mosaik2_indextask *task) {
	// duration 0.2 ms
	// lock the lockfile to make all other forked processes wait this process finishp

	FILE *lockfile_file = m_fopen( md->lock_filename, "r");
	int lockfile_fd = fileno(lockfile_file);
	if(lockfile_fd == -1) {
		fprintf(stderr, "could not open lock fd\n");
		exit(EXIT_FAILURE);
	}
	if (flock(lockfile_fd, LOCK_EX) == -1) {
	  if (errno == EWOULDBLOCK) {
			fprintf(stderr,"lockfile is already locked\n");
			exit(EXIT_FAILURE);
		}
	}

	FILE *imagecolors_file = m_fopen( md->imagecolors_filename, "a");
	FILE *imagestddev_file = m_fopen( md->imagestddev_filename, "a");
	FILE *imagedims_file = m_fopen( md->imagedims_filename, "a");
	FILE *image_index_file = m_fopen( md->image_index_filename, "a");
	FILE *filenames_file = m_fopen( md->filenames_filename, "a");
	FILE *filenames_index_file = m_fopen( md->filenames_index_filename, "a");
	FILE *filehashes_file = m_fopen( md->filehashes_filename, "a");
	FILE *timestamps_file = m_fopen( md->timestamps_filename, "a");
	FILE *filesizes_file = m_fopen( md->filesizes_filename, "a");
	FILE *tiledims_file = m_fopen( md->tiledims_filename, "a");
	FILE *invalid_file = m_fopen( md->invalid_filename, "a");
	FILE *duplicates_file = m_fopen( md->duplicates_filename, "a");
	FILE *lastmodified_file = m_fopen( md->lastmodified_filename, "w");
	FILE *tileoffsets_file = m_fopen( md->tileoffsets_filename, "a");


	char null_value='\0';
	char ff_value=0xFF;
	char new_line='\n';

	//TODO check if everything is written to disk
	off_t image_offset = ftello(imagecolors_file);
	m_fwrite(&image_offset, sizeof(off_t), image_index_file);
	m_fwrite(task->colors, RGB*task->total_tile_count, imagecolors_file);
	m_fwrite(task->colors_stddev, RGB*task->total_tile_count, imagestddev_file);

	m_fwrite(&task->width, sizeof(int), imagedims_file);
	m_fwrite(&task->height, sizeof(int), imagedims_file);

	off_t filenames_offset = ftello(filenames_file);
	m_fwrite(&filenames_offset, sizeof(off_t), filenames_index_file);
	m_fwrite(task->filename, strlen(task->filename), filenames_file);
	m_fwrite(&new_line, 1, filenames_file);

	m_fwrite(task->hash, MD5_DIGEST_LENGTH, filehashes_file);

	m_fwrite(&task->lastmodified, sizeof(time_t), timestamps_file);

	m_fwrite(&task->filesize, sizeof(size_t), filesizes_file);

	m_fwrite(&task->tile_x_count, sizeof(char), tiledims_file);
	m_fwrite(&task->tile_y_count, sizeof(char), tiledims_file);
	
	m_fwrite(&null_value, 1, invalid_file);

	m_fwrite(&null_value, 1, duplicates_file);

	// the content is just written for updating ".lastmodified"s modified timestamp
	m_fwrite(&task->lastmodified, sizeof(time_t),  lastmodified_file);

	m_fwrite(&ff_value, sizeof(ff_value), tileoffsets_file);
	m_fwrite(&ff_value, sizeof(ff_value), tileoffsets_file);


	m_fclose( imagecolors_file );
	m_fclose( imagestddev_file );
	m_fclose( imagedims_file );
	m_fclose( image_index_file );
	m_fclose( filenames_file );
	m_fclose( filenames_index_file );
	m_fclose( filehashes_file );
	m_fclose( timestamps_file );
	m_fclose( filesizes_file );
	m_fclose( tiledims_file );
	m_fclose( invalid_file );
	m_fclose( duplicates_file );
	m_fclose( lastmodified_file );
	m_fclose( tileoffsets_file );


	//print_usage("unflock");
	if (flock(lockfile_fd, LOCK_UN) == -1) {
		fprintf(stderr,"flock error");
		exit(EXIT_FAILURE);
	}
	m_fclose( lockfile_file );

}

void mosaik2_index_add_tiler_pid(mosaik2_context *ctx, pid_t pid) {
	if(pid < 1) {
		fprintf(stderr, "pid of forked process has an invalid value\n");
		exit(EXIT_FAILURE);
	}
	int inserted=0;
	for(int i=0; i < MOSAIK2_CONTEXT_MAX_TILER_PROCESSES; i++) {
		if(ctx->pids[i] == 0) {
			ctx->pids[i] = pid;
			ctx->current_tiler_processes++;
			inserted = 1;
			break;
		}
	}
	if(inserted==0) {
		fprintf(stderr, "could not save pid, this should not have happend\n");
		exit(EXIT_FAILURE);
	}
}

void mosaik2_index_clean_tiler_pids(mosaik2_context *ctx) {
	//print_usage("clean0");
		usleep(10000);
	//print_usage("clean1");
	
	for(int i=0;i<10; i++) {
		if(ctx->pids[i]>0) {
			int status;
			//fprintf(stdout, "%i check pid %i\n", getpid(),ctx->pids[i]);
			//pid_t child_pid = waitpid(-1, &status, WUNTRACED | WCONTINUED);
			
			pid_t child_pid = waitpid(ctx->pids[i], &status, WNOHANG);
			//fprintf(stdout, "%i check pid %i return_pid:%i\n", getpid(),ctx->pids[i], child_pid);

			if(child_pid == 0)
				continue; //still running
			if(child_pid == ctx->pids[i]) {

			//if(WIFEXITED(status)) printf("%i exited:%d\n", getpid(), child_pid);
			//if(WIFSIGNALED(status)) printf("%i wifsignaled %d\n", getpid(), child_pid);
				
				ctx->pids[i]=0;
				ctx->current_tiler_processes--;
				
			} else if(child_pid == -1) {
				fprintf(stdout, "%i check pid %i\n", getpid(),ctx->pids[i]);
				ctx->pids[i]=0;
				ctx->current_tiler_processes--;
			} else {
				fprintf(stderr, "unexpected waitpid code of child process: %i\n", child_pid);
				exit(EXIT_FAILURE);
			}
		}
	}
	//print_usage("clean2");
			/*if(child_pid==-1)perror("child_pid");
			fprintf(stdout, "%i waitpid return value:%i,child_pid:%i\n", getpid(), ctx->pids[i], child_pid);
			continue;
			if(child_pid == ctx->pids[i]) {
				fprintf(stdout, "%i child state has changed ctx->pids[%i]:%i child_pid:%i\n", getpid(), i, ctx->pids[i], child_pid);
			} else if(child_pid==0) {
				fprintf(stdout, "%i child state has not changed\n", getpid());
			}
			//fprintf(stdout, "%i check pid2 %i %i\n", getpid(),ctx->pids[i]);
			if(child_pid == -1) {
				fprintf(stdout,"%i waitpid=-1\n", getpid());
				ctx->pids[i]=0;
				ctx->current_tiler_processes--;
				continue;
				//exit(EXIT_FAILURE);
			}
			//printf("%i waitpid() returned: PID=%ld; status=0x%04x (%d,%d)\n",
			//	getpid(),(long) child_pid,
			//	(unsigned int) status, status >> 8, status & 0xff);	
			if(WIFEXITED(status)) {
				ctx->pids[i]=0;
				ctx->current_tiler_processes--;
				//printf("%i exited:%d\n", getpid(), child_pid);
			}
			if(WIFSIGNALED(status))
				printf("%i wifsignaled %d\n", getpid(), child_pid);
		}*/
}

/*void signal_handler(int signal) {
	if(signal == SIGINT) {
		printf("SIGINT catched, do not exit\n");
	}
	if(signal==SIGUSR1) {
		fprintf(stdout, "parent SIGUSR1 received\n");
	}
}
*/

	static void sigHandler(int sig) {
	//	printf("%i %i sigHandler sig:%i\n", getppid(), getpid(), sig);
		if(sig==SIGINT) {
			exiting = 1;
		}
	}
