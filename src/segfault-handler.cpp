#include <fcntl.h>
#include <nan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <node.h>
#include <uv.h>

#ifdef _WIN32
#include "../includes/StackWalker.h"
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <assert.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#endif
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace v8;
using namespace Nan;

#define STDERR_FD 2

#ifdef _WIN32
#define CLOSE _close
#define GETPID _getpid
#define O_FLAGS _O_CREAT | _O_APPEND | _O_WRONLY
#define OPEN _open
#define S_FLAGS _S_IWRITE
#define SEGFAULT_HANDLER LONG CALLBACK segfault_handler(PEXCEPTION_POINTERS exceptionInfo)
#define SNPRINTF _snprintf
#define WRITE _write
#else
#define CLOSE close
#define GETPID getpid
#define O_FLAGS O_CREAT | O_APPEND | O_WRONLY
#define OPEN open
#define S_FLAGS S_IRUSR | S_IRGRP | S_IROTH
#define SEGFAULT_HANDLER static void segfault_handler(int sig, siginfo_t *si, void *unused)
#define SNPRINTF snprintf
#define WRITE write
#endif

#define BUFF_SIZE 128

Eternal<ArrayBuffer> segfaultError;
void *segfaultErrorData;
size_t segfaultErrorByteLength;

struct callback_helper {

  struct callback_args {

    v8::Persistent<Function, v8::CopyablePersistentTraits<Function> >* callback;
    char **stack;
    size_t stack_size;
    int signo;
    long addr;
    std::mutex mutex;
    std::condition_variable cond;

    callback_args(v8::Persistent<Function, v8::CopyablePersistentTraits<Function> >* callback, char **stack, size_t stack_size, int signo, long addr) :
      callback(callback),
      stack(stack),
      stack_size(stack_size),
      signo(signo),
      addr(addr)
      {}

    ~callback_args() {
      free(stack);
    }
  };

  uv_async_t* handle;
  v8::Persistent<Function, v8::CopyablePersistentTraits<Function> > callback;

  callback_helper(Handle<Function> func) {
    Isolate* isolate = Isolate::GetCurrent();
    // set the function reference
    callback.Reset(isolate, func);

    // create the callback handle
    handle = (uv_async_t*) malloc(sizeof (uv_async_t));

    // initialize the handle
    uv_async_init(uv_default_loop(), handle, make_callback);
  }

  ~callback_helper() {
    // reset the function reference
    callback.Reset();

    // close the callback handle
    uv_close((uv_handle_t*) handle, close_callback);
  }

  void send(char **stack, size_t stack_size, int signo, long addr) {
    // create the callback arguments
    callback_args* args = new callback_args(&callback, stack, stack_size, signo, addr);

    // set the handle data so these args are accessible to make_callback
    handle->data = (void *) args;

    // directly execute the callback if we're on the main thread,
    // otherwise have uv send it and await the mutex
    if (Isolate::GetCurrent()) {
      make_callback(handle);
    } else {
      // lock the callback mutex
      std::unique_lock<std::mutex> lock(args->mutex);

      // trigger the async callback
      uv_async_send(handle);

      // wait for it to finish
      args->cond.wait(lock);
    }

    // free the callback args
    delete args;
  }

  static void close_callback(uv_handle_t* handle) {
    // free the callback handle
    free(handle);
  }

  static void make_callback(uv_async_t* handle) {
    Isolate* isolate = Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    struct callback_args* args = (struct callback_args*) handle->data;

    // lock the mutex
    std::unique_lock<std::mutex> lock(args->mutex);

    // collect all callback arguments
    Handle<Value> segfaultErrorLocal = segfaultError.Get(Isolate::GetCurrent());
    Handle<Value> argv[] = {
      segfaultErrorLocal,
      Number::New(isolate, segfaultErrorByteLength)
    };

    // execute the callback function on the main threaod
    Local<Function>::New(isolate, *args->callback)->Call(isolate->GetCurrentContext()->Global(), sizeof(argv)/sizeof(argv[0]), argv);

    // broadcast that we're done with the callback
    args->cond.notify_one();
  }
};

struct callback_helper* callback;

char logPath[BUFF_SIZE];

/* static void buildFileName(char sbuff[BUFF_SIZE], int pid) {
  time_t  now;

  // Construct a filename
  time(&now);
  if (logPath[0] != '\0') {
    SNPRINTF(sbuff, BUFF_SIZE, "%s", logPath);
  } else {
    SNPRINTF(sbuff, BUFF_SIZE, "stacktrace-%d-%d.log", (int)now, pid);
  }
} */

SEGFAULT_HANDLER {
  long address;
  long code;
  char **array; // Array to store backtrace symbols
  size_t size;       // To store the size of the stack backtrace
  // char    sbuff[BUFF_SIZE];
  // int     n;          // chars written to buffer
  // int     fd;
  // int     pid;

  // pid = GETPID();

  /* buildFileName(sbuff, pid);
  fd = OPEN(sbuff, O_FLAGS, S_FLAGS); */

  #ifdef _WIN32
    address = (long)exceptionInfo->ExceptionRecord->ExceptionAddress;
    code = (long)exceptionInfo->ExceptionRecord->ExceptionCode;
    if (code < 0x80000000) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
  #else
    address = (long)si->si_addr;
    code = (long)si->si_signo;
  #endif

  /* // Write the header line
  n = SNPRINTF(
    sbuff,
    BUFF_SIZE,
    "PID %d received SIGSEGV for address: 0x%lx\n",
    pid,
    address
  );

  if(fd > 0) {
    if (WRITE(fd, sbuff, n) != n) {
      fprintf(stderr, "NodeSegfaultHandlerNative: Error writing to file\n");
    }
  }
  fprintf(stderr, "%s", sbuff); */

  #ifdef _WIN32
    // will generate the stack trace and write to fd and stderr
    StackWalker sw;
    sw.ShowCallstack();

    array = new char *[32]; // Array to store backtrace symbols
    array[0] = (char *)sw.error.c_str();
    size = 1;
  #else
    // Write the Backtrace
    void *symbols[32];
    size = backtrace(symbols, 32);
    // if(fd > 0) backtrace_symbols_fd(symbols, size, fd);
    array = backtrace_symbols(symbols, size);
  #endif

  size_t index = 0;
  for (size_t i = 0; i < size; i++) {
    char *e = (char *)(array[i]);
    size_t length = strlen(e);
    memcpy((char *)segfaultErrorData + index, e, length);
    ((char *)segfaultErrorData)[index + length] = '\n';
    index += length + 1;
  }
  segfaultErrorByteLength = index;

  // CLOSE(fd);

  if (callback) {
  // execute the callback and wait until it has completed
    callback->send(array, size, code, address);

  // release the callback
    delete callback;
  }

  #ifdef _WIN32
    return EXCEPTION_EXECUTE_HANDLER;
  #endif
}

// create some stack frames to inspect from CauseSegfault
#ifndef _WIN32
__attribute__ ((noinline))
#else
__declspec(noinline)
#endif
void segfault_stack_frame_1()
{
  // DDOPSON-2013-04-16 using the address "1" instead of "0" prevents a nasty compiler over-optimization
  // When using "0", the compiler will over-optimize (unless using -O0) and generate a UD2 instruction
  // UD2 is x86 for "Invalid Instruction" ... (yeah, they have a valid code that means invalid)
  // Long story short, we don't get our SIGSEGV.  Which means no pretty demo of stacktraces.
  // Instead, you see "Illegal Instruction: 4" on the console and the program stops.

  int *foo = (int*)1;
  fprintf(stderr, "NodeSegfaultHandlerNative: about to dereference NULL (will cause a SIGSEGV)\n");
  *foo = 78; // trigger a SIGSEGV

}

#ifndef _WIN32
__attribute__ ((noinline))
#else
__declspec(noinline)
#endif
void segfault_stack_frame_2(void) {
  // use a function pointer to thwart inlining
  void (*fn_ptr)() = segfault_stack_frame_1;
  fn_ptr();
}

NAN_METHOD(CauseSegfault) {
  // use a function pointer to thwart inlining
  void (*fn_ptr)() = segfault_stack_frame_2;
  fn_ptr();
}

NAN_METHOD(RegisterHandler) {
  // if passed a path, we'll set the log name to whatever is provided
  // this will allow users to use the logs in error reporting without redirecting
  // sdterr

  if (info.Length() > 0) {
    for (int i = 0; i < info.Length(); i++) {
      if (info[i]->IsString()) {
        String::Utf8Value utf8Value(info[i]->ToString());

        // need to do a copy to make sure the string doesn't become a dangling pointer
        int len = utf8Value.length();
        len = len > BUFF_SIZE ? BUFF_SIZE : len;

        strncpy(logPath, *utf8Value, len);
        logPath[127] = '\0';

      } else if (info[i]->IsFunction()) {
        if (callback) {
          // release previous callback
          delete callback;
        }

        // create the new callback object
        callback = new callback_helper(Handle<Function>::Cast(info[i]));

      }
    }
  }

  #ifdef _WIN32
    AddVectoredExceptionHandler(1, segfault_handler);
  #else
    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = segfault_handler;
    sa.sa_flags   = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
  #endif
}

extern "C" {
  NAN_MODULE_INIT(init) {
    Local<ArrayBuffer> segfaultErrorLocal = ArrayBuffer::New(Isolate::GetCurrent(), 1024 * 1024);
    segfaultError.Set(Isolate::GetCurrent(), segfaultErrorLocal);
    segfaultErrorData = segfaultErrorLocal->GetContents().Data();
    segfaultErrorByteLength = segfaultErrorLocal->ByteLength();

    Nan::SetMethod(target, "registerHandler", RegisterHandler);
    Nan::SetMethod(target, "causeSegfault", CauseSegfault);
  }

  NODE_MODULE(segfault_handler, init)
}
