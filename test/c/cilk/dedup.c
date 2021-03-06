#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <cilk/cilk.h>
#include <cilk/cilk_api.h>

#define CHUNKSIZE (2)
#define N (CHUNKSIZE * 16)
#define QSIZE (7)
#define FREE (999)
#define EOT (9999) //end-of-tasks

#define LOOP_SIZE 1000000
#define TIME

double timespec_to_ms(struct timespec *ts)
{
  return ts->tv_sec*1000.0 + ts->tv_nsec/1000000.0;
}

void print (int x[]) {

    printf("------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < N; i ++ ) {
      printf("%d,", x[i]);
    }
    printf("\n");

}//print

void dedup_S4 (int x[], int y[], volatile int q[]) {


    unsigned rptr = 0;
    unsigned pos = 0;
    while ((pos = q[rptr]) != EOT) {
      if (pos != FREE) {
	  y[pos] = x[pos];
	  y[pos + 1] = x[pos + 1];

	  q[rptr] = FREE;
	  rptr = (rptr + 1) % ((1<<QSIZE)-1);
      }
    } //while not end-of-tasks

}//dedup_S4


void dedup_S3 (int chunk[], int pos, int wptr, volatile int q[]) {

    //compress with RLE
    chunk[1] = 2;

    //enqueue task of writing out
    q[wptr] = pos;

}//dedup_S3


void dedup_S2 (int x[], int pos, int wptr, volatile int q[]) {

    int* chunk = &x[pos];

    if (chunk[0] == chunk[1]) {
      cilk_spawn dedup_S3 (chunk, pos, wptr, q);
    } else {
      q[wptr] = pos;
    }
    cilk_sync;
}//dedup_S2



void dedup (int x[], int y[], volatile int q[]) {

    int pos = 0;
    int outpos = 0;
    int done = 0;
    unsigned wptr = 0;

    cilk_spawn dedup_S4 (x, y, q);
	    
    while (x[pos] != 0) {
            
      if(q[wptr] == FREE) {
	cilk_spawn dedup_S2 (x, pos, wptr, q);
	pos += CHUNKSIZE;
	wptr = (wptr+1) % ((1<<QSIZE)-1);
      }

    }//while not end of buffer

    // Tell everyone to stop
    q[wptr] = EOT;
    // wait until all spawned tasks complete
    cilk_sync;
    // Reset queue for additional runs
    q[wptr] = FREE;

}//dedup

int main (int argc, char *argv[]) {

  int a[N+1] = {97, 97, 50, 50,105,105,108,108, 99, 99,	\
		112,112,115,115,107,107,108,108,114,114,\
		121,121,118,118,109,109, 99,112,106,110,\
		98,112,0};
    volatile int q[1<<QSIZE];
    for (int i = 0; i < (1<<QSIZE); i++) {
      q[i] = FREE;
    }
  //    int a[N + 1];
    int out[N];
    /*
    a[0] = a[1] = 'a';
    for (int i = 2; i < N; i++) {
            a[i] = rand() % ('z' - 'a') + 'a' ;
    }

    a[N] = 0;
    */
    // If we've got a parameter, assume it's the number of workers to be used
    printf("%d %d\n", 1<<QSIZE, (1<<QSIZE)-1);
    if (argc > 1) {
      // Values less than 1, or parameters that aren't numbers aren't allowed
      if (atoi(argv[1]) < 1)
	{
	  printf("Usage: fib [workers]\n");
	  return 1;
	}

      // Set the number of workers to be used
#ifdef TIME
      __cilkrts_set_param("nworkers", argv[1]);
#endif
    }

    print (a);
#ifdef TIME
  struct timespec start_time, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif  
  for (int i=0;i<LOOP_SIZE;i++) {
    dedup (a, out, q);
  }
#ifdef TIME
  clock_gettime(CLOCK_MONOTONIC, &end_time);
  double time_ms = timespec_to_ms(&end_time) - timespec_to_ms(&start_time);
  float time_ns = time_ms / LOOP_SIZE * 1000000;
  printf("Calculated in %.3f ns using %d workers.\n",
  	 time_ns, __cilkrts_get_nworkers());
#endif
    print (out);
    


    return 0;
}//main


