#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <pthread.h>
#include <semaphore.h>
#include <linux/ptrace.h>
#include <sys/ptrace.h>
#include <bpf/bpf.h>
#include "realtime-tp.h"  // Include the tracepoint header

#define NUM_THREADS 2
#define ITERATIONS 100000

volatile double position = 0.0;
sem_t mutex_123456;

typedef struct {
    int x;
    int y;
} Point;

typedef struct {
    int numPoints;
    Point *points;
} Polygon;

int isPointInPolygon(Point point, const Polygon *polygon) {
    int i, j, c = 0;
    for (i = 0, j = polygon->numPoints - 1; i < polygon->numPoints; j = i++) {
        if (((polygon->points[i].y > point.y) != (polygon->points[j].y > point.y)) &&
            (point.x < (polygon->points[j].x - polygon->points[i].x) * (point.y - polygon->points[i].y) /
                       (polygon->points[j].y - polygon->points[i].y) + polygon->points[i].x)) {
            c = !c;
        }
    }
    return c;
}

struct timespec AddTimespecByNs(struct timespec ts, int64_t ns) {
    ts.tv_nsec += ns;

    while (ts.tv_nsec >= 1000000000) {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000;
    }

    while (ts.tv_nsec < 0) {
        --ts.tv_sec;
        ts.tv_nsec += 1000000000;
    }

    return ts;
}

// BPF Probes
BPF_HASH(start, uint32_t);
BPF_HASH(lock_duration, uint32_t);

static void trace_mutex_lock(void* ctx) {
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t ts = bpf_ktime_get_ns();
    start.update(&tid, &ts);
}

static void trace_mutex_unlock(void* ctx) {
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t* tsp = start.lookup(&tid);
    if (tsp == 0) {
        return; // missed start
    }
    uint64_t delta = bpf_ktime_get_ns() - *tsp;
    lock_duration.increment(tid, delta);
    start.delete(&tid);
}

void *control_thread_func(void *arg) {
    // Register the BCC probes for mutex_lock and mutex_unlock functions
    bpf_attach_uprobe("realtime", "mutex_lock", trace_mutex_lock);
    bpf_attach_uprobe("realtime", "mutex_unlock", trace_mutex_unlock);
    struct timespec start_time, end_time;
    long long total_time = 0;
    struct timespec next_wakeup_time_;
    int64_t period_ns_ = 1000000;
    while (true) {
        // Acquire the semaphore to enter the critical section
        sem_wait(&mutex_123456);

        // Record the start time
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // Read the current position
        double current_position = position;

        // Control algorithm - PID controller
        double error = 0.0;  // Calculate the he system. To identify the mutex related to your real-time application, you can use the following steps:error based on desired and current position
        double control_signal = error * 0.1;  // Adjust the control signal

        int num_points = 10000;
        Point points[num_points];
        for (int i = 0; i < num_points; ++i) {
            Point point = {5, 5};
            points[i] = point;
        }
        Polygon polygon = {num_points, points};
        Point base_point = {0, 0};
        isPointInPolygon(base_point, &polygon);

        // Actuator - Apply the control signal
        // ...

        // Update the position
        position += control_signal;

        // Record the end time
        clock_gettime(CLOCK_MONOTONIC, &end_time);

        // Calculate the elapsed time in nanoseconds
        long long elapsed_time = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL +
                                 (end_time.tv_nsec - start_time.tv_nsec);

        // Accumulate the total time
        total_time += elapsed_time;

        // Trace semaphore release operation
                tracepoint(realtime, semaphore_release);

        // Release the semaphore to exit the critical section
        sem_post(&mutex_123456);


        // Calculate the average time per iteration
        double average_time = (double) total_time / ITERATIONS;

        printf("Control Thread - Average time per iteration: %.2f ns\n", average_time);

        next_wakeup_time_ = AddTimespecByNs(next_wakeup_time_, period_ns_);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup_time_, NULL);
    }

    return NULL;
}

void *sensor_thread_func(void *arg) {
    struct timespec start_time, end_time;
    long long total_time = 0;
    bpf_attach_uprobe("realtime", "mutex_lock", trace_mutex_lock);
    bpf_attach_uprobe("realtime", "mutex_unlock", trace_mutex_unlock);
    
        // Acquire the semaphore to enter the critical section
        sem_wait(&mutex_123456);

        // Record the start time
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        // Sensor - Read the position
        double current_position = position;

        // Record the end time
        clock_gettime(CLOCK_MONOTONIC, &end_time);

        // Calculate the elapsed time in nanoseconds
        long long elapsed_time = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL +
                                 (end_time.tv_nsec - start_time.tv_nsec);

        // Accumulate the total time
        total_time += elapsed_time;

        // Trace semaphore get operation
                tracepoint(realtime, semaphore_get);

        // Release the semaphore to exit the critical section
        sem_post(&mutex_123456);

    // Calculate the average time per iteration
    double average_time = (double) total_time / ITERATIONS;

    printf("Sensor Thread - Average time per iteration: %.2f ns\n", average_time);

    return NULL;
}

int main() {
    // Lock memory to prevent paging
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("mlockall");
        return 1;
    }

    // Initialize the semaphore
    sem_init(&mutex_123456, 0, 1);  // 1 indicates the initial value of the semaphore

    // Create real-time attributes for the threads
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // Set the scheduling policy and priority for the threads
    struct sched_param param;
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

    // Set the priority for the control thread
    param.sched_priority = 80;
    pthread_attr_setschedparam(&attr, &param);

    // Create and start the control thread
    pthread_t control_thread;
    pthread_create(&control_thread, &attr, control_thread_func, NULL);

    // Set the priority for the sensor thread
    param.sched_priority = 70;
    pthread_attr_setschedparam(&attr, &param);

    // Create and start the sensor thread
    pthread_t sensor_thread;
    pthread_create(&sensor_thread, &attr, sensor_thread_func, NULL);

    // Wait for the threads to complete
    pthread_join(control_thread, NULL);
    pthread_join(sensor_thread, NULL);

    // Destroy the semaphore
    sem_destroy(&mutex_123456);

    return 0;
}

