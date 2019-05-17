
static int launch_valgrind(int argc, char *argv[], char *envp[]) {
	debug_printf(2, "command=%s\n", "__MONITOR_VALGRIND__");
	FILE *valgrind_error_pipe = popen("__MONITOR_VALGRIND__", "w");
	int valgrind_error_fd = 2;
	if (valgrind_error_pipe) {
		fwrite(tar_data, sizeof tar_data[0],  sizeof tar_data/sizeof tar_data[0], valgrind_error_pipe);
    	fflush(valgrind_error_pipe);
		setbuf(valgrind_error_pipe, NULL);			
		valgrind_error_fd = (int)fileno(valgrind_error_pipe);
	} else {
		debug_printf(2, "popen failed");
		return __real_main(argc, argv, envp);
	}
	setenvd("DCC_VALGRIND_RUNNING", "1");
	
	char fd_buffer[64];
	snprintf(fd_buffer, sizeof fd_buffer, "--log-fd=%d", valgrind_error_fd);
	char *valgrind_command[] = {
		"/usr/bin/valgrind",
		fd_buffer,
		"-q",
		"--vgdb=yes",
		"--leak-check=__LEAK_CHECK_YES_NO__",
		"--suppressions=__SUPRESSIONS_FILE__",
		"--max-stackframe=16000000",
		"--partial-loads-ok=no",
		"--malloc-fill=0xbe",
		"--free-fill=0xbe",
		 "--vgdb-error=1"
	};

	int valgrind_command_len = sizeof valgrind_command / sizeof valgrind_command[0];
	char *valgrind_argv[valgrind_command_len + argc + 1];
	for (int i = 0; i < valgrind_command_len; i++)
		valgrind_argv[i] = valgrind_command[i];
	for (int i = 0; i < argc; i++)
		valgrind_argv[valgrind_command_len + i] = argv[i];
		
	valgrind_argv[valgrind_command_len + argc] = NULL;
	for (int i = 0; i < valgrind_command_len + argc; i++)
		debug_printf(3, "valgrind_argv[%d] = %s\n", i, valgrind_argv[i]);
	
	execvp("/usr/bin/valgrind", valgrind_argv);

	debug_printf(2, "execvp failed");
	return __real_main(argc, argv, envp);
}


static void __dcc_start(void) {
	char *debug_level_string = getenv("DCC_DEBUG");
	if (debug_level_string) {
		debug_level = atoi(debug_level_string);
	}
	debug_printf(2, "__dcc_start debug_level=%d\n", debug_level);
	
	setenvd("DCC_SANITIZER", "__SANITIZER__");
	setenvd("DCC_PATH", "__PATH__");

	setenvd_int("DCC_PID", getpid());
	
	signal(SIGABRT, __dcc_signal_handler);
	signal(SIGSEGV, __dcc_signal_handler);
	signal(SIGINT, __dcc_signal_handler);
	signal(SIGXCPU, __dcc_signal_handler);
	signal(SIGXFSZ, __dcc_signal_handler);
	signal(SIGFPE, __dcc_signal_handler);
	signal(SIGILL, __dcc_signal_handler);
	signal(SIGPIPE, __dcc_signal_handler);
	signal(SIGUSR1, __dcc_signal_handler);
	clear_stack();
}

void __dcc_error_exit(void) {
	debug_printf(2, "__dcc_error_exit()\n");

#if __N_SANITIZERS__ > 1
	cleanup_before_exit();
#endif

#if __SANITIZER__ != VALGRIND
	// use kill instead of exit or _exit because
	// exit or _exit keeps executing sanitizer code - including perhaps superfluous output
	// but not with valgrind which will catch signal and start gdb
	// SIGPIPE avoids killed message from bash
	signal(SIGPIPE, SIG_DFL); 
	kill(getpid(), SIGPIPE);   
#endif

	_exit(1);
}

// is __asan_on_error  address sanitizer only??
//
// intercept ASAN explanation
void __asan_on_error() NO_SANITIZE;

void __asan_on_error() {
	debug_printf(2, "__asan_on_error\n");

	char *report = "";
#if __SANITIZER__ == ADDRESS && __CLANG_VERSION_MAJOR__ >= 6
	extern char *__asan_get_report_description();
	extern int __asan_report_present();
	if (__asan_report_present()) {
		report = __asan_get_report_description();
	}
#endif
	char report_description[8192];
	snprintf(report_description, sizeof report_description, "DCC_ASAN_ERROR=%s", report);
	putenvd(report_description);
	
	_explain_error();
	// not reached
}

// intercept ASAN explanation
void _Unwind_Backtrace(void *a, ...) {
	debug_printf(2, "_Unwind_Backtrace\n");
	_explain_error();
}

#if __SANITIZER__ == ADDRESS
char *__asan_default_options() {

	// NOTE setting detect_stack_use_after_return here stops
	// clear_stack pre-initializing stack frames to 0xbe
	
	return "verbosity=0:print_stacktrace=1:halt_on_error=1:detect_leaks=__LEAK_CHECK_1_0__:max_malloc_fill_size=4096000:quarantine_size_mb=16:verify_asan_link_order=0:detect_stack_use_after_return=__STACK_USE_AFTER_RETURN__";
}
#endif

#if __SANITIZER__ == MEMORY
char *__msan_default_options() {
	return "verbosity=0:print_stacktrace=1:halt_on_error=1:detect_leaks=__LEAK_CHECK_1_0__";
}
#endif

void __ubsan_on_report(void) {
	debug_printf(2, "__ubsan_on_report\n");

#if __UNDEFINED_BEHAVIOUR_SANITIZER_IN_USE__ && __CLANG_VERSION_MAJOR__ >= 7 
	char *OutIssueKind;
	char *OutMessage;
	char *OutFilename;
	unsigned int OutLine;
	unsigned int OutCol;
	char *OutMemoryAddr;
	extern void __ubsan_get_current_report_data(char **OutIssueKind, char **OutMessage, char **OutFilename, unsigned int *OutLine, unsigned int *OutCol, char **OutMemoryAddr);

	__ubsan_get_current_report_data(&OutIssueKind, &OutMessage, &OutFilename, &OutLine, &OutCol, &OutMemoryAddr);

	// buffer + putenv is ugly - but safer?
	char buffer[6][128];
	snprintf(buffer[0], sizeof buffer[0], "DCC_UBSAN_ERROR_KIND=%s", OutIssueKind);
	snprintf(buffer[1], sizeof buffer[1], "DCC_UBSAN_ERROR_MESSAGE=%s", OutMessage);
	snprintf(buffer[2], sizeof buffer[2], "DCC_UBSAN_ERROR_FILENAME=%s", OutFilename);
	snprintf(buffer[3], sizeof buffer[3], "DCC_UBSAN_ERROR_LINE=%u", OutLine);
	snprintf(buffer[4], sizeof buffer[4], "DCC_UBSAN_ERROR_COL=%u", OutCol);
	snprintf(buffer[5], sizeof buffer[5], "DCC_UBSAN_ERROR_MEMORYADDR=%s", OutMemoryAddr);
	for (int i = 0; i < sizeof buffer/sizeof buffer[0]; i++) 
		putenv(buffer[i]);
#endif
	_explain_error();
	// not reached
}

#if __UNDEFINED_BEHAVIOUR_SANITIZER_IN_USE__
char *__ubsan_default_options() {
	return "verbosity=0:print_stacktrace=1:halt_on_error=1:detect_leaks=__LEAK_CHECK_1_0__";
}
#endif

static void set_signals_default(void) {
	debug_printf(2, "set_signals_default()\n");
	signal(SIGABRT, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGXCPU, SIG_DFL);
	signal(SIGXFSZ, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGPIPE, SIG_DFL); 
	signal(SIGUSR1, SIG_IGN); 
}

static void __dcc_signal_handler(int signum) {
	set_signals_default();
#if __N_SANITIZERS__ > 1
#if __I_AM_SANITIZER1__
	if (signum == SIGPIPE) {
		if (!synchronization_terminated) {
			stop_sanitizer2();
		} else {
			__dcc_error_exit();
		}
	} else if (signum == SIGUSR1) {
		debug_printf(2, "received SIGUSR\n");
		__dcc_error_exit();
	}
#else
	__dcc_error_exit();
#endif
#endif
	
	char signum_buffer[64];
	snprintf(signum_buffer, sizeof signum_buffer, "DCC_SIGNAL=%d", (int)signum);
	putenvd(signum_buffer); // less likely? to trigger another error than direct setenv
	
	_explain_error();// not reached
}

#if __EMBED_SOURCE__
static char *run_tar_file = "python3 -E -c \"import os,sys,tarfile,tempfile\n\
with tempfile.TemporaryDirectory() as temp_dir:\n\
 try:\n\
  tarfile.open(fileobj=sys.stdin.buffer, mode='r|xz').extractall(temp_dir)\n\
 except Exception:\n\
  pass\n\
 os.chdir(temp_dir)\n\
 exec(open('start_gdb.py').read())\n\
\"";
#endif

static void _explain_error(void) {
#if __N_SANITIZERS__ > 1 && __I_AM_SANITIZER1__
	stop_sanitizer2();
#endif
	// if a program has exhausted file descriptors then we need to close some to run gdb etc,
	// so as a precaution we close a pile of file descriptors which may or may not be open
	for (int i = 4; i < 32; i++)
		close(i);

#ifdef __linux__
    // ensure gdb can ptrace binary
	// https://www.kernel.org/doc/Documentation/security/Yama.txt
	prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
#endif

#if !__EMBED_SOURCE__
	debug_printf(2, "running %s\n", "__PATH__");
	system("__PATH__");
#else
	debug_printf(2, "running %s\n", run_tar_file);
	FILE *python_pipe = popen(run_tar_file, "w");
	size_t n_items = sizeof tar_data/sizeof tar_data[0];
	size_t items_written = fwrite(tar_data, sizeof tar_data[0], n_items, python_pipe);
	if (items_written != n_items) {
		debug_printf(1, "fwrite bad return %d returned %d expected\n", (int)items_written, (int)n_items);
	}
	fclose(python_pipe);
#endif
	__dcc_error_exit();
}

#if !__STACK_USE_AFTER_RETURN__ 
static void _memset_shim(void *p, int byte, size_t size) NO_SANITIZE
#if __has_attribute(noinline)
__attribute__((noinline))
#endif
#if __has_attribute(optnone)
__attribute__((optnone))
;
#endif

int scanf(const char *format, ...) {
    va_list arg;
    va_start(arg, format);
    int n = vscanf(format, arg);
    va_end(arg);
	quick_clear_stack();
    return n;
}

int printf(const char *format, ...) {
    va_list arg;
    va_start(arg, format);
    int n = vprintf(format, arg);
    va_end(arg);
	quick_clear_stack();
    return n;
}


// hack to initialize (most of) stack to 0xbe
// so uninitialized variables are more obvious

static void clear_stack(void) {
	char a[4096000];
	debug_printf(3, "initialized %p to %p\n", a, a + sizeof a);
	_memset_shim(a, 0xbe, sizeof a);
}

static void quick_clear_stack(void) {
	char a[256000];
	debug_printf(3, "initialized %p to %p\n", a, a + sizeof a);
	_memset_shim(a, 0xbe, sizeof a);
}


// hide memset in a function with optimization turned off
// to avoid calls being removed by optimizations
static void _memset_shim(void *p, int byte, size_t size) {
	memset(p, byte, size);
}
#else
static void clear_stack(void) {
}
static void quick_clear_stack(void) {
}
#endif

static void setenvd(char *n, char *v) {
	setenv(n, v, 1);
	debug_printf(2, "setenv %s=%s\n", n, v);
}

static void setenvd_int(char *n, int v) {
	char buffer[64];
	snprintf(buffer, sizeof buffer, "%d", v);
	setenvd(n, buffer);
}

static void putenvd(char *s) {
	putenv(s);
	debug_printf(2, "putenv %s\n", s);
}

static int debug_printf(int level, const char *format, ...) {
	if (level > debug_level) {
		return 0;
	}
#if __N_SANITIZERS__ > 1
	fprintf(debug_stream ? debug_stream : stderr, "__WHICH_SANITIZER__: ");
#if __I_AM_SANITIZER2__
	fprintf(debug_stream ? debug_stream : stderr, "\t");
#endif
#endif
    va_list arg;
    va_start(arg, format);
    int n = vfprintf(debug_stream ? debug_stream : stderr, format, arg);
    va_end(arg);
    return n;
}