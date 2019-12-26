

#include "mrloop.h"
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <time.h>

#define DBG if(0)

static int mrfd;
static char tmp[32*1024];
static int tmplen = 32*1024;
static int num_sqes = 0;

/*
static void print_buffer( char* b, int len ) {
  for ( int z = 0; z < len; z++ ) {
    printf( "%02x ",(int)b[z]);
    //printf( "%c",b[z]);
  }
  printf("\n");
}
*/

mr_loop_t *mr_create_loop(mr_signal_cb *sig) {

  signal(SIGINT, sig);
  signal(SIGTERM, sig);
  signal(SIGHUP, sig);
  signal(SIGPIPE, SIG_IGN);

  mr_loop_t *loop = calloc( 1, sizeof(mr_loop_t) );
  loop->ring = calloc( 1, sizeof(struct io_uring) );
  if ( io_uring_queue_init(128, loop->ring, 0) ) {
    perror("io_uring_setup");
    printf("Loop creation failed\n");
    return NULL;
  }
  // Timer 
  loop->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (loop->timer_fd < 0) { perror("timerfd_create"); return NULL; }
  loop->thead = NULL;
  loop->timerEvent = calloc( 1, sizeof(event_t) );
  loop->timerEvent->type = TIMER_EV;

  loop->fd = loop->ring->ring_fd;
  loop->writeDataEvent = calloc( 1, sizeof(event_t) );
  loop->writeDataEvent->type = WRITE_DATA_EV;
  for (int i = 0; i < MAX_CONN; i++) {
    loop->readEvents[i] = calloc( 1, sizeof(event_t) );
    loop->readDataEvents[i] = calloc( 1, sizeof(event_t) );
    loop->readEvents[i]->type = READ_EV;
    loop->readDataEvents[i]->type = READ_DATA_EV;

    //loop->writeEvents[i] = calloc( 1, sizeof(event_t) );
    //loop->writeEvents[i]->type = WRITE_EV;
  }

  return loop;
}


void mr_free(mr_loop_t *loop) {
  free(loop->ring);
  free(loop->writeDataEvent);
  for (int i = 0; i < MAX_CONN; i++) {
    free(loop->readEvents[i]);
    free(loop->readDataEvents[i]);
  }
  free(loop); 
}

void mr_stop(mr_loop_t *loop) {
  loop->stop = 1;
  //printf("ELMELOOP mr_stop called\n");
}

void _urpoll( mr_loop_t *loop, int fd, event_t *ev ) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_poll_add( sqe, fd, POLLIN );
  sqe->user_data = (unsigned long)ev;
  io_uring_submit(loop->ring);

}

void mr_add_write_callback( mr_loop_t *loop, mr_write_cb *cb, void *conn, int fd ) {

  event_t *ev  = calloc( 1, sizeof(event_t) );
  ev->type = WRITE_EV;
  ev->fd = fd;
  ev->user_data = conn;
  ev->wcb = cb;

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_poll_add( sqe, fd, POLLOUT );
  sqe->user_data = (unsigned long)ev;
  io_uring_submit(loop->ring);
  
}
void mr_add_read_callback( mr_loop_t *loop, mr_write_cb *cb, void *conn, int fd ) {

  event_t *ev  = calloc( 1, sizeof(event_t) );
  ev->type = READ_CB_EV;
  ev->fd = fd;
  ev->user_data = conn;
  ev->wcb = cb;

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_poll_add( sqe, fd, POLLIN );
  sqe->user_data = (unsigned long)ev;
  io_uring_submit(loop->ring);
  
}

void _addTimer( mr_loop_t *loop, event_t *ev ) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  if (!sqe) { printf("child: get sqe failed\n"); return; }
  io_uring_prep_poll_add( sqe, ev->fd, POLLIN );
  io_uring_sqe_set_data(sqe, ev);
  io_uring_submit(loop->ring);
}


void _read( mr_loop_t *loop, event_t *ev ) {
  event_t *rdev = loop->readDataEvents[ev->fd];

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_readv(sqe, rdev->fd, &(rdev->iov), 1, 0);
  sqe->user_data = (unsigned long)rdev;

  io_uring_submit(loop->ring); 

}

// TODO use 5.5 async accept4 with SOCK_NONBLOCK
void _accept( mr_loop_t *loop, event_t *ev ) {


  struct sockaddr_in addr;
  socklen_t len;
  int cfd = accept(ev->fd, (struct sockaddr*)&addr, &len);

  if (fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFD,0) | O_NONBLOCK) == -1) {
    printf("set non blocking error : %d %s...\n", errno, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (cfd != -1) {
  //while ((cfd = accept(ev->fd, (struct sockaddr*)&addr, &len)) != -1) {
    //if (cfd >= MAX_CONN) { close(cfd); break; }

    mrfd = cfd;
    char *buf;
    int buflen;
    ev->user_data = ev->acb( cfd, &buf, &buflen);

    // Setup the read event
    event_t *rev  = loop->readEvents[cfd];
    event_t *rdev = loop->readDataEvents[cfd];
    rev->fd = cfd;
    //rev->user_data = ev->user_data;
    rdev->fd = cfd;
    rdev->rcb = ev->rcb;
    rdev->user_data = ev->user_data;
    rdev->iov.iov_base = buf;
    rdev->iov.iov_len = buflen;
    _urpoll( loop, cfd, rev );

    //readCompleteEvents[cfd].fd = cfd;
    //readCompleteEvents[cfd].cb = on_read_complete;

  } else {
    printf( " mrloop: _accept - error: %s\n", strerror(errno) );
    exit(-1);
  }
  // Keep listening
  _urpoll( loop, ev->fd, ev );

}

static int numtes = 0; // DELME
static void mr_process_time_event( mr_loop_t *loop ) {
  mr_time_event_t *te = loop->thead;
  if ( te == NULL ) return;

  numtes -= 1;
  //printf("DELME mr_process_time_event num %d\n",numtes);
  te->cb(te->user_data);

  loop->thead = te->next;
  //  
  //te->cb = func;
  //te->user_data = user_data;

}

// TODO if this is negative then consume the entry
static void *mr_set_timeout( mr_loop_t *loop, struct __kernel_timespec *ts ) {

  //printf("DELME mr_set_timeout\n");
  mr_time_event_t *te = loop->thead;
  //if ( te == NULL ) printf("DELME mr_set_timeout te null\n");
  if ( te == NULL ) return NULL;

  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);

/*
  printf("DELME now: %ld %ld\n",now.tv_sec,now.tv_nsec);
  int n = 0;
  while (te) {
    printf("DELME te%d: %ld %ld\n",n,te->sec,te->ms);
    n += 1;
    te = te->next;
  }
  te = loop->thead;
*/

  //printf("DELME mr_set_timeout te %ld %ld\n",te->sec,te->ms);
  long sec = te->sec - now.tv_sec;
  long ms  = te->ms  - now.tv_nsec/1e6;
  //printf("DELME mr_set_timeout %ld %ld\n",sec,ms);

  //if ( sec < 0 || (sec == 0 && ms < 0) ) { printf(" mr set timeout found neg time\n"); }
  if ( sec < 0 || (sec == 0 && ms <= 0) ) { mr_process_time_event( loop ); mr_set_timeout( loop, ts ); return loop->thead; }

  ts->tv_sec  = sec;
  if ( ms < 0 ) {
    ts->tv_sec -= 1;
    ts->tv_nsec = (ms + 1000) * 1e6;
  }
  else ts->tv_nsec = ms * 1e6;
 
  //printf("DELME mr_set_timeout returning %p\n",te);
  return te;
}


void mr_run( mr_loop_t *loop ) {
  struct io_uring_cqe *cqe;
  
  while ( 1 ) {

    //printf("ELMELOOP top of loop\n");
    if ( loop->stop ) return;
    //TODO wait_cqe returns nonzero on error

    // Check for waiting time events
    if ( loop->thead ) {
      struct __kernel_timespec ts; 

      // This processes any time events that have triggered and returns non zero if we have to wait
      if ( mr_set_timeout( loop, &ts ) ) {
        if ( loop->stop ) return;
    	  io_uring_wait_cqe_timeout(loop->ring, &cqe, &ts);
        //printf("ELMELOOP te - after wait_cqe_timeout\n");
      } else {
        if ( loop->stop ) return;
    	  io_uring_wait_cqe(loop->ring, &cqe);
        //printf("ELMELOOP te - after wait_cqe\n");
      }
    } else {
    	io_uring_wait_cqe(loop->ring, &cqe);
      //printf("ELMELOOP after wait_cqe\n");
    }

    if ( !cqe ) {
      mr_process_time_event( loop );
      if ( loop->stop ) return;
      io_uring_cqe_seen(loop->ring, cqe);
      continue; 
    }

    event_t *ev = (event_t*)cqe->user_data;
    if ( !ev ) {
      io_uring_cqe_seen(loop->ring, cqe);
      continue; 
    }

    if ( ev->type == TIMER_EV ) {
      uint64_t value;
      int rd = read(ev->fd, &value, 8);
      if ( ev->tcb(ev->user_data) ) _addTimer(loop, ev);
    }
    if ( ev->type == LISTEN_EV ) {
      _accept( loop, ev );
    }
    if ( ev->type == READ_EV ) {
      _read(loop, ev);
    }
    if ( ev->type == READ_CB_EV ) {
      ev->wcb(ev->user_data, ev->fd);
      free(ev);
    }
    if ( ev->type == WRITE_EV ) {
      ev->wcb(ev->user_data, ev->fd);
      free(ev);
    }
    if ( ev->type == READ_DATA_EV ) {

        //printf("ELMELOOP read data before res %d\n",cqe->res);
        // TODO Allow user to return a value saying close this?
        //      I think we add a close callback and we close.
        ev->rcb( ev->user_data, ev->fd, cqe->res, ev->iov.iov_base );
        if ( loop->stop ) return;
        //_urpoll( loop, ev->fd, loop->readEvents[ev->fd] );
        if ( cqe->res > 0 ) {
          _urpoll( loop, ev->fd, loop->readEvents[ev->fd] );
          //printf("ELMELOOP read data res %d\n",cqe->res);
        } else {
          //printf("ELMELOOP read data res 0\n");
          // User will call close? Do we need to free anything? TODO
        }
        //io_uring_submit(loop->ring);  // poll submits
        num_sqes = 0;
    }
    if ( ev->type == WRITE_DATA_EV ) {
      ev->wdcb(ev->user_data);
    }
    if ( ev->type == TIMER_ONCE_EV ) {
      uint64_t value;
      ev->tcb(ev->user_data);
      if ( ev->fd ) {
        int rd = read(ev->fd, &value, 8);
        close(ev->fd);
      }
    }

    //if ( ev2 ) ev2->cb(ev2->fd, cqe->res);
    io_uring_cqe_seen(loop->ring, cqe);
    if ( loop->stop ) return;
  }
}

int mr_add_timer( mr_loop_t *loop, double seconds, mr_timer_cb *cb, void *user_data ) {

  int secs, ms;
  secs = ms = 0;
  if ( seconds < 1 ) ms = seconds * 1000;
  else secs = seconds;
  struct itimerspec exp = {
    .it_interval = { secs, ms * 1000000},
    .it_value = { secs, ms * 1000000 },
  };
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (tfd < 0) { perror("timerfd_create"); return -1; }
  if (timerfd_settime(tfd, 0, &exp, NULL)) { perror("timerfd_settime"); close(tfd); return -1; }
  
  event_t *ev = malloc( sizeof(event_t) );
  ev->type = TIMER_EV;
  ev->fd = tfd;
  ev->tcb = cb;
  ev->user_data = user_data;  

  _addTimer(loop, ev);
  return 0;
}

int mr_tcp_server( mr_loop_t *loop, int port, mr_accept_cb *cb, mr_read_cb *rcb) { //, char *buf, int buflen ) {
  int listen_fd;
  struct sockaddr_in servaddr;
  int flags = 1;
  //socklen_t len = sizeof(struct sockaddr_in);

  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(port);

  if ((listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) {
      printf("socket error : %d ...\n", errno);
      exit(EXIT_FAILURE);
  }

  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
  setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
  //if (error != 0) perror("setsockopt");

  if (bind(listen_fd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr)) == -1) {
      printf("bind error : %d ...\n", errno);
      exit(EXIT_FAILURE);
  }

  if (listen(listen_fd, 32) == -1) {
      printf("listen error : %d ...\n", errno);
      exit(EXIT_FAILURE);
  }

  // TODO free this in mr_free(loop)
  event_t *ev = malloc( sizeof(event_t) );
  ev->type = LISTEN_EV;
  ev->fd = listen_fd;
  ev->acb = cb;
  ev->rcb = rcb;

  _urpoll( loop, listen_fd, ev );

  return 0;
}

int mr_connect( mr_loop_t *loop, char *addr, int port, mr_read_cb *rcb) {

  int fd, ret, on = 1;
  struct sockaddr_in sa;

  if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) {
    printf("Error creating socket: %s\n", strerror(errno));
    return -1;
  }

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_aton(addr, &sa.sin_addr) == 0) {
    struct hostent *he;

    he = gethostbyname(addr);
    if (he == NULL) {
      //anetSetError(err, "can't resolve: %s\n", addr);
      close(fd);
      //return ANET_ERR;
      return -1;
    }
    memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
  }

  ret = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
  if ( ret == -1 && errno != EINPROGRESS ) {
    printf("Error connecting socket: %s\n", strerror(errno));
    close(fd);
    return -1;
  }

  // Setup the read event
  event_t *rev  = loop->readEvents[fd];
  event_t *rdev = loop->readDataEvents[fd];
  rev->fd = fd;
  rev->user_data = 0;
  _urpoll( loop, fd, rev );

  rdev->fd = fd;
  rdev->rcb = rcb;
  rdev->user_data = 0;
  rdev->iov.iov_base = tmp;
  rdev->iov.iov_len = tmplen;

  return fd;
}

// TODO What if we have events in flight?
void mr_close( mr_loop_t *loop, int fd ) {
  close(fd);
}

void mr_flush( mr_loop_t *loop ) {
  io_uring_submit(loop->ring); 
  num_sqes = 0;
}

void mr_writev( mr_loop_t *loop, int fd, struct iovec *iovs, int cnt ) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_writev(sqe, fd, iovs, cnt, 0);
  sqe->user_data = 0;
  num_sqes += 1;
  if ( num_sqes > 64 ) { io_uring_submit(loop->ring); num_sqes = 0; }

}

void mr_writevf( mr_loop_t *loop, int fd, struct iovec *iovs, int cnt ) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_writev(sqe, fd, iovs, cnt, 0);
  sqe->user_data = 0;
  io_uring_submit(loop->ring); 
  num_sqes = 0;

}

void mr_writevcb( mr_loop_t *loop, int fd, struct iovec *iovs, int cnt, void *user_data, mr_write_done_cb *cb ) {

  event_t *ev = malloc( sizeof(event_t) );
  ev->type = WRITE_DATA_EV;
  ev->fd = fd;
  ev->wdcb = cb;
  ev->user_data = user_data;

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_writev(sqe, fd, iovs, cnt, 0);
  sqe->user_data = (unsigned long)ev;
  num_sqes += 1;
  if ( num_sqes > 64 ) { io_uring_submit(loop->ring); num_sqes = 0; }

}

//static inline void io_uring_prep_write_fixed(struct io_uring_sqe *sqe, int fd, const void *buf, unsigned nbytes, off_t offset, int buf_index)

void mr_write( mr_loop_t *loop, int fd, const void *buf, unsigned nbytes, off_t offset ) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(loop->ring);
  io_uring_prep_write_fixed(sqe, fd, buf, nbytes, offset, 0);
  sqe->user_data = 0;
  io_uring_submit(loop->ring); 
  num_sqes = 0;

}


// TODO Insert from back since later events should be later times
static void mr_insert_time_event( mr_loop_t *loop, mr_time_event_t *te ) {

  //numtes += 1;
  //printf("mr_insert_time_event num %d\n",numtes);
  //if ( loop->thead == NULL ) { printf("mr_insert to head\n"); }
  if ( loop->thead == NULL ) { loop->thead = te; return; }

  mr_time_event_t *n = loop->thead;
  mr_time_event_t *p = NULL;
  while ( n ) {
    if ( n->sec > te->sec || (n->sec == te->sec && n->ms > te->ms) ) {
      te->next = n;
      if ( p ) p->next = te;
      else loop->thead = te;
      return;       
    }
    p = n;
    n = n->next;
  }

  p->next = te;
}

void mr_call_after( mr_loop_t *loop, mr_timer_cb *func, uint64_t milliseconds, void *user_data ) {

  // Setup time_event
  mr_time_event_t *te = calloc( 1, sizeof(mr_time_event_t) );
  te->cb = func;
  te->user_data = user_data;

  // Now + delay
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  te->sec = t.tv_sec + milliseconds/1000;
  te->ms  = t.tv_nsec/1e6 + (milliseconds%1000);
  if ( te->ms >= 1000 ) {
    te->sec += 1; te->ms -= 1000;
  }

  //printf(" nowsec %d tsec %d \n", t.tv_sec, te->sec );
  //printf(" nowms %d tms %d huh %d \n", t.tv_nsec/1e6, te->ms,(milliseconds%1000));

  // Insert it into the LL in time order
  mr_insert_time_event( loop, te );    

}

void mr_call_soon( mr_loop_t *loop, mr_timer_cb *cb, void *user_data ) {

  // Setup time_event
  mr_time_event_t *te = calloc( 1, sizeof(mr_time_event_t) );
  te->cb = cb;
  te->user_data = user_data;

  // Now 
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  //te->sec = t.tv_sec;
  //te->ms  = t.tv_nsec/1e6;
  te->sec = 0;
  te->ms  = 0;

  //printf("DELME bef insert\n");
  mr_insert_time_event( loop, te );    

}


