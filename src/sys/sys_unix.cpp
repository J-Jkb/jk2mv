#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <pwd.h>
#include <pthread.h>
#include <fenv.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <execinfo.h>
#ifdef __GNU_LIBRARY__
#include <gnu/libc-version.h>
#endif

#ifdef MACOS_X
#include <mach-o/dyld.h>
#endif

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "con_local.h"

#include <mv_setup.h>

//=============================================================================

// Used to determine CD Path
static char cdPath[MAX_OSPATH];

// Used to determine local installation path
static char installPath[MAX_OSPATH];

// Used to determine where to store user-specific files
static char homePath[MAX_OSPATH];

/*
================
Sys_Milliseconds
================
*/
/* base time in seconds, that's our origin
   timeval:tv_sec is an int:
   assuming this wraps every 0x7fffffff - ~68 years since the Epoch (1970) - we're safe till 2038 */
unsigned long sys_timeBase = 0;
/* current time in ms, using sys_timeBase as origin
   NOTE: sys_timeBase*1000 + curtime -> ms since the Epoch
     0x7fffffff ms - ~24 days
   although timeval:tv_usec is an int, I'm not sure wether it is actually used as an unsigned int
     (which would affect the wrap period) */
int curtime;
int Sys_Milliseconds (bool baseTime)
{
    struct timeval tp;

    gettimeofday(&tp, NULL);

    if (!sys_timeBase)
    {
        sys_timeBase = tp.tv_sec;
        return tp.tv_usec/1000;
    }

    curtime = (tp.tv_sec - sys_timeBase)*1000 + tp.tv_usec/1000;

    static int sys_timeBase = curtime;
    if (!baseTime)
    {
        curtime -= sys_timeBase;
    }

    return curtime;
}

int Sys_Milliseconds2( void )
{
    return Sys_Milliseconds(false);
}

qboolean Sys_Mkdir( const char *path )
{
    int result = mkdir( path, 0750 );

    if( result != 0 )
        return (qboolean)(errno == EEXIST);

    return qtrue;
}

//============================================

#define	MAX_FOUND_FILES	0x1000

// bk001129 - new in 1.26
static void Sys_ListFilteredFiles( const char *basedir, char *subdirs, char *filter, const char **list, int *numfiles ) {
	char		search[MAX_OSPATH], newsubdirs[MAX_OSPATH];
	char		filename[MAX_OSPATH];
	DIR			*fdir;
	struct dirent *d;
	struct stat st;

	if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
		return;
	}

	if (strlen(subdirs)) {
		Com_sprintf( search, sizeof(search), "%s/%s", basedir, subdirs );
	}
	else {
		Com_sprintf( search, sizeof(search), "%s", basedir );
	}

	if ((fdir = opendir(search)) == NULL) {
		return;
	}

	while ((d = readdir(fdir)) != NULL) {
		Com_sprintf(filename, sizeof(filename), "%s/%s", search, d->d_name);
		if (stat(filename, &st) == -1)
			continue;

		if (st.st_mode & S_IFDIR) {
			if (Q_stricmp(d->d_name, ".") && Q_stricmp(d->d_name, "..")) {
				if (strlen(subdirs)) {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s/%s", subdirs, d->d_name);
				}
				else {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s", d->d_name);
				}
				Sys_ListFilteredFiles( basedir, newsubdirs, filter, list, numfiles );
			}
		}
		if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
			break;
		}
		Com_sprintf( filename, sizeof(filename), "%s/%s", subdirs, d->d_name );
		if (!Com_FilterPath( filter, filename, qfalse ))
			continue;
		list[ *numfiles ] = CopyString( filename );
		(*numfiles)++;
	}

	closedir(fdir);
}

// bk001129 - in 1.17 this used to be
// char **Sys_ListFiles( const char *directory, const char *extension, int *numfiles, qboolean wantsubs )
const char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs )
{
	struct dirent *d;
	// char *p; // bk001204 - unused
	DIR		*fdir;
	qboolean dironly = wantsubs;
	char		search[MAX_OSPATH];
	int			nfiles;
	const char	**listCopy;
	const char	*list[MAX_FOUND_FILES];
	//int			flag; // bk001204 - unused
	int			i;
	struct stat st;

	int			extLen;

	if (filter) {

		nfiles = 0;
		Sys_ListFilteredFiles( directory, "", filter, list, &nfiles );

		list[ nfiles ] = 0;
		*numfiles = nfiles;

		if (!nfiles)
			return NULL;

		listCopy = (const char **)Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ),TAG_FILESYS,qfalse );
		for ( i = 0 ; i < nfiles ; i++ ) {
			listCopy[i] = list[i];
		}
		listCopy[i] = NULL;

		return listCopy;
	}

	if ( !extension)
		extension = "";

	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		dironly = qtrue;
	}

	extLen = strlen( extension );

	// search
	nfiles = 0;

	if ((fdir = opendir(directory)) == NULL) {
		*numfiles = 0;
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL) {
		Com_sprintf(search, sizeof(search), "%s/%s", directory, d->d_name);
		if (stat(search, &st) == -1)
			continue;
		if ((dironly && !(st.st_mode & S_IFDIR)) ||
			(!dironly && (st.st_mode & S_IFDIR)))
			continue;

		if (*extension) {
			if ( strlen( d->d_name ) < strlen( extension ) ||
				Q_stricmp(
					d->d_name + strlen( d->d_name ) - strlen( extension ),
					extension ) ) {
				continue; // didn't match
			}
		}

		if ( nfiles == MAX_FOUND_FILES - 1 )
			break;
		list[ nfiles ] = CopyString( d->d_name );
		nfiles++;
	}

	list[ nfiles ] = 0;

	closedir(fdir);

	// return a copy of the list
	*numfiles = nfiles;

	if ( !nfiles ) {
		return NULL;
	}

	listCopy = (const char **)Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ),TAG_FILESYS,qfalse );
	for ( i = 0 ; i < nfiles ; i++ ) {
		listCopy[i] = list[i];
	}
	listCopy[i] = NULL;

	return listCopy;
}

void	Sys_FreeFileList( const char **list ) {
	int		i;

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( (void *)list[i] );
	}

	Z_Free( list );
}

char *Sys_Cwd( void )
{
	static char cwd[MAX_OSPATH];

	if ( !getcwd( cwd, sizeof( cwd ) - 1 ) ) {
        return "";
	}

	cwd[MAX_OSPATH-1] = 0;

	return cwd;
}

void Sys_SetDefaultCDPath(const char *path)
{
	Q_strncpyz(cdPath, path, sizeof(cdPath));
}

char *Sys_DefaultCDPath(void)
{
        return cdPath;
}

void Sys_SetDefaultInstallPath(const char *path)
{
	Q_strncpyz(installPath, path, sizeof(installPath));
}

#if defined(MACOS_X)
static char *last_strstr(const char *haystack, const char *needle)
{
    if (*needle == '\0')
        return (char *) haystack;

    char *result = NULL;
    for (;;) {
        char *p = (char *)strstr(haystack, needle);
        if (p == NULL)
            break;
        result = p;
        haystack = p + 1;
    }

    return result;
}
#endif // MACOS_X

#if !defined(MACOS_X) && defined(INSTALLED)
char *Sys_LinuxGetInstallPrefix() {
    static char path[MAX_OSPATH];
    int i;

    readlink("/proc/self/exe", path, sizeof(path));

    // from: /usr/local/bin/jk2mvmp
    // to: /usr/local
    for (i = 0; i < 2; i++) {
        char *l = strrchr(path, '/');
        if (!l) {
            break;
        }

        *l = 0;
    }

    return path;
}
#endif

char *Sys_DefaultInstallPath(void)
{
    if (*installPath) {
        return installPath;
    } else {
#if defined(MACOS_X) && defined(INSTALLED)
        // Inside the app...
        static char path[MAX_OSPATH];
        char *override;

        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size)) {
            return Sys_Cwd();
        }

        override = last_strstr(path, "MacOS");
        if (!override) {
            return Sys_Cwd();
        }

        override[5] = 0;
        return path;
#elif !defined(MACOS_X) && defined(INSTALLED)
        static char path[MAX_OSPATH];

        Com_sprintf(path, sizeof(path), "%s/share/jk2mv", Sys_LinuxGetInstallPrefix());
        return path;
#else
		return Sys_Cwd();
#endif
    }
}

void Sys_SetDefaultHomePath(const char *path)
{
	Q_strncpyz(homePath, path, sizeof(homePath));
}

// if dedicated, i think it's a good idea to default fs_homepath to fs_basepath on unix, too
// remember it's only the default value
char *Sys_DefaultHomePath(void)
{
#if defined(INSTALLED)
	char *p;

        if (*homePath)
            return homePath;

	if ((p = getenv("HOME")) != NULL) {
		Q_strncpyz(homePath, p, sizeof(homePath));
#ifdef MACOS_X
		Q_strcat(homePath, sizeof(homePath), "/Library/Application Support/jk2mv");
#else
		Q_strcat(homePath, sizeof(homePath), "/.jk2mv");
#endif
		if (mkdir(homePath, 0777)) {
			if (errno != EEXIST)
				Sys_Error("Unable to create directory \"%s\", error is %s(%d)\n", homePath, strerror(errno), errno);
		}
		return homePath;
	}
	return ""; // assume current dir
#else
    return Sys_Cwd();
#endif
}

// try to find assets from /Applications (Appstore JK2) or Steam
// if not found try to find it in the same directory this app is
char *Sys_DefaultAssetsPath() {
#if defined(MACOS_X) && defined(INSTALLED)
    static char path[MAX_OSPATH];
    char *override;

    // AppStore version
    if (access("/Applications/Jedi Knight II.app/Contents/base/assets0.pk3", F_OK) != -1) {
        return "/Applications/Jedi Knight II.app/Contents";
    }

    // Steam version
    if (access(va("%s/Library/Application Support/Steam/steamapps/common/Jedi Outcast/Jedi Knight II.app/Contents/base/assets0.pk3", getenv("HOME")), F_OK) != -1) {
        Q_strncpyz(path, va("%s/Library/Application Support/Steam/steamapps/common/Jedi Outcast/Jedi Knight II.app/Contents", getenv("HOME")), sizeof(path));
        return path;
    }

    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size)) {
        return NULL;
    }

    override = last_strstr(path, ".app/");
    if (!override) {
        return NULL;
    }
    *override = 0;

    override = last_strstr(path, "/");
    if (!override) {
        return NULL;
    }

    strcpy(override, "/Jedi Knight II.app/Contents");
    if (access(va("%s/base/assets0.pk3", path), F_OK) == -1) {
        return NULL;
    }

    return path;
#else
    return NULL;
#endif
}

qboolean stdin_active = qtrue;
qboolean stdinIsATTY = qfalse;
int crashlogfd;
extern void		Sys_SigHandler( int signal );
static void		Sys_SigHandlerFatal(int sig, siginfo_t *info, void *context);
static Q_NORETURN void Sys_CrashLogger(int fd, int argc, char *argv[]);

void Sys_PlatformInit( int argc, char *argv[] )
{
	int		crashfd[2];
	pid_t	crashpid;

	if (pipe(crashfd) == -1) {
		perror("pipe()");
		goto skip_crash;
	}

	if ((crashpid = fork()) == -1) {
		perror("fork()");
		goto skip_crash;
	}

	if (crashpid == 0) {
		close(crashfd[1]);
		Sys_CrashLogger(crashfd[0], argc, argv);
	} else {
		close(crashfd[0]);
		crashlogfd = crashfd[1];
	}
skip_crash:
	struct sigaction act = { 0 };

	act.sa_handler = Sys_SigHandler;

	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGHUP);
	sigaddset(&act.sa_mask, SIGINT);
	sigaddset(&act.sa_mask, SIGTERM);

	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);

	act.sa_handler = NULL;
	act.sa_sigaction = Sys_SigHandlerFatal;
	act.sa_flags = SA_SIGINFO | SA_NODEFER;

	sigaction(SIGQUIT, &act, NULL);
	sigaction(SIGILL, &act, NULL);
	sigaction(SIGABRT, &act, NULL);
	sigaction(SIGFPE, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGBUS, &act, NULL);
	sigaction(SIGSYS, &act, NULL);
	sigaction(SIGTRAP, &act, NULL);

	const char* term = getenv( "TERM" );

    if (isatty( STDIN_FILENO ) && !( term && ( !strcmp( term, "raw" ) || !strcmp( term, "dumb" ) ) ))
		stdinIsATTY = qtrue;
    else
        stdinIsATTY = qfalse;
}

void Sys_PlatformExit( void )
{
}

int Sys_PID( void )
{
	return getpid( );
}

//============================================

int Sys_GetProcessorId( void )
{
	return CPUID_GENERIC;
}

char *Sys_GetCurrentUser( void )
{
	struct passwd *p;

	if ( (p = getpwuid( getuid() )) == NULL ) {
		return "player";
	}
	return p->pw_name;
}

// from ioq3 requires sse
// i do not care about processors without sse
#if defined(__i386__) || defined(__amd64__)
#if idx64
  #define EAX "%%rax"
  #define EBX "%%rbx"
  #define ESP "%%rsp"
  #define EDI "%%rdi"
#else
  #define EAX "%%eax"
  #define EBX "%%ebx"
  #define ESP "%%esp"
  #define EDI "%%edi"
#endif

static unsigned char ssemask[16] __attribute__((aligned(16))) = {
	"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00\x00\x00"
};

void Sys_SnapVector(vec3_t vec) {
	__asm__ volatile
	(
		"movaps (%0), %%xmm1\n"
		"movups (%1), %%xmm0\n"
		"movaps %%xmm0, %%xmm2\n"
		"andps %%xmm1, %%xmm0\n"
		"andnps %%xmm2, %%xmm1\n"
		"cvtps2dq %%xmm0, %%xmm0\n"
		"cvtdq2ps %%xmm0, %%xmm0\n"
		"orps %%xmm1, %%xmm0\n"
		"movups %%xmm0, (%1)\n"
		:
		: "r" (ssemask), "r" (vec)
		: "memory", "%xmm0", "%xmm1", "%xmm2"
	);

}
#else
void Sys_SnapVector(float *v) {
    v[0] = nearbyintf(v[0]);
    v[1] = nearbyintf(v[1]);
    v[2] = nearbyintf(v[2]);
}
#endif

/*
==================
Sys_Sleep

Block execution for msec or until input is recieved.
==================
*/
void Sys_Sleep( int msec )
{
	if( msec == 0 )
		return;

	if( stdinIsATTY )
	{
		fd_set fdset;

		FD_ZERO(&fdset);
		FD_SET(STDIN_FILENO, &fdset);
		if( msec < 0 )
		{
			select(STDIN_FILENO + 1, &fdset, NULL, NULL, NULL);
		}
		else
		{
			struct timeval timeout;

			timeout.tv_sec = msec/1000;
			timeout.tv_usec = (msec%1000)*1000;
			select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout);
		}
	}
	else
	{
		// With nothing to select() on, we can't wait indefinitely
		if( msec < 0 )
			msec = 10;

		usleep( msec * 1000 );
	}
}

/*
=================
Sys_UnloadModuleLibrary
=================
*/
void Sys_UnloadModuleLibrary(void *dllHandle) {
	if (!dllHandle) {
		return;
	}

	dlclose(dllHandle);
}

/*
=================
Sys_LoadModuleLibrary

Used to load a module (jk2mpgame, cgame, ui) so/dylib
=================
*/
void *Sys_LoadModuleLibrary(const char *name, qboolean mvOverride, intptr_t(QDECL **entryPoint)(int, ...), intptr_t(QDECL *systemcalls)(intptr_t, ...)) {
	void (*dllEntry)(intptr_t(*syscallptr)(intptr_t, ...));
	char filename[MAX_QPATH];
	const char *path, *filePath;
	void *libHandle;

	Com_sprintf(filename, sizeof(filename), "%s_" ARCH_STRING "." LIBRARY_EXTENSION, name);

	if (!mvOverride) {
		path = Cvar_VariableString("fs_basepath");
		filePath = FS_BuildOSPath(path, NULL, filename);

		Com_DPrintf("Loading module: %s...", filePath);
		libHandle = dlopen(filePath, RTLD_NOW);
		if (!libHandle) {
			Com_DPrintf(" failed: %s\n", dlerror());
			path = Cvar_VariableString("fs_homepath");
			filePath = FS_BuildOSPath(path, NULL, filename);

			Com_DPrintf("Loading module: %s...", filePath);
			libHandle = dlopen(filePath, RTLD_NOW);
			if (!libHandle) {
				Com_DPrintf(" failed: %s\n", dlerror());
				return NULL;
			} else {
				Com_DPrintf(" success!\n");
			}
		} else {
			Com_DPrintf(" success!\n");
		}
	} else {
		char lpath[1024];

#if !defined(MACOS_X) && defined(INSTALLED)
		Com_sprintf(lpath, sizeof(lpath), "%s/lib/%s", Sys_LinuxGetInstallPrefix(), filename);
#else
		path = Cvar_VariableString("fs_basepath");
		Com_sprintf(lpath, sizeof(lpath), "%s/%s", path, filename);
#endif

		Com_DPrintf("Loading module: %s...", lpath);
		libHandle = dlopen(lpath, RTLD_NOW);
		if (!libHandle) {
			Com_DPrintf(" failed: %s\n", dlerror());
			return NULL;
		} else {
			Com_DPrintf(" success!\n");
		}
	}

	dllEntry = (void (*)(intptr_t (*)(intptr_t,...))) dlsym(libHandle, "dllEntry");
	*entryPoint = (intptr_t(*)(int,...))dlsym(libHandle, "vmMain");
	if ( !*entryPoint ) {
		Com_DPrintf("Could not find vmMain in %s\n", filename);
		dlclose(libHandle);
		return NULL;
	}

	if (!dllEntry) {
		Com_DPrintf("Could not find dllEntry in %s\n", filename);
		dlclose(libHandle);
		return NULL;
	}

	dllEntry(systemcalls);
	return libHandle;
}

void Sys_SetTaskbarState(void *win_handle, tbstate_t state, uint64_t current, uint64_t total) {
	// TODO: OSX Dock support
}

int Sys_FLock(int fd, flockCmd_t cmd, qboolean nb) {
	struct flock l = { 0 };

	l.l_whence = SEEK_SET;
	l.l_pid = getpid();

	switch (cmd) {
	case FLOCK_SH: l.l_type = F_RDLCK; break;
	case FLOCK_EX: l.l_type = F_WRLCK; break;
	case FLOCK_UN: l.l_type = F_UNLCK; break;
	}

	return fcntl(fd, nb ? F_SETLK : F_SETLKW, &l);
}

/*
==============================================================

Asynchronous Crash Logging

==============================================================
*/

static qboolean Sys_CrashWrite(int fd, const void *buf, size_t len);
static void Sys_SigHandlerFatal(int sig, siginfo_t *info, void *context) {
	static volatile sig_atomic_t signalcaught = 0;
	ucontext_t *ucontext = (ucontext_t *)context;

	if (!signalcaught) {
		signalcaught = 1;

		Sys_CrashWrite(crashlogfd, info, sizeof(*info));
		Sys_CrashWrite(crashlogfd, &(ucontext->uc_mcontext), sizeof(mcontext_t));
		Sys_CrashWrite(crashlogfd, &consoleLog, sizeof(consoleLog));
		close(crashlogfd);
	}

	// reraise the signal with default handler
	struct sigaction act = { 0 };
	act.sa_handler = SIG_DFL;
	sigemptyset(&act.sa_mask);
	sigaction(sig, &act, NULL);
	raise(sig);
}

static qboolean Sys_CrashWrite(int fd, const void *buf, size_t len) {
	unsigned	count = 0;

	while (count < len) {
		int ret = write(fd, buf, len - count);

		if (ret == 0) {
			return qfalse;
		}
		if (ret == -1) {
			if (errno == EINTR) {
				continue;		// a signal was caught
			} else {
				perror("Sys_CrashWrite()");
				return qfalse;
			}
		}

		count += ret;
	}

	return qtrue;
}

static qboolean Sys_CrashRead(int fd, void *buf, size_t len) {
	unsigned	count = 0;

	while (count < len) {
		int ret = read(fd, buf, len - count);

		if (ret == 0) {
			return qfalse;
		}
		if (ret == -1) {
			if (errno == EINTR) {
				continue;		// a signal was caught
			} else {
				perror("Sys_CrashRead()");
				return qfalse;
			}
		}

		count += ret;
	}

	return qtrue;
}

static const char *Sys_DescribeSignalCode(int signal, int code) {
	switch (code) {
	case SI_USER:		return "kill";
	case SI_KERNEL:		return "sent by the kernel";
	case SI_QUEUE: 		return "sigqueue";
	case SI_TKILL: 		return "tkill or tgkill";
	}

	switch (signal) {
	case SIGILL:
		switch (code) {
		case ILL_ILLOPC:	return "illegal opcode";
		case ILL_ILLOPN:	return "illegal operand";
		case ILL_ILLADR:	return "illegal addressing mode";
		case ILL_ILLTRP:	return "illegal trap";
		case ILL_PRVOPC:	return "priviledged opcode";
		case ILL_PRVREG:	return "priviledged register";
		case ILL_COPROC:	return "coprocessor error";
		case ILL_BADSTK:	return "internal stack error";
		}
		break;
	case SIGFPE:
		switch (code) {
		case FPE_INTDIV:	return "integer divide by zero";
		case FPE_INTOVF:	return "integer overflow";
		case FPE_FLTDIV:	return "floating-point divide by zero";
		case FPE_FLTOVF:	return "floating-point overflow";
		case FPE_FLTUND:	return "floating-point underflow";
		case FPE_FLTRES:	return "floating-point inexact result";
		case FPE_FLTINV:	return "floating-point invalid operation";
		case FPE_FLTSUB:	return "subscript out of range";
		}
		break;
	case SIGSEGV:
		switch(code) {
		case SEGV_MAPERR:	return "address not mapped to object";
		case SEGV_ACCERR:	return "invalid permission for mapped object";
		}
		break;
	case SIGBUS:
		switch (code) {
		case BUS_ADRALN:	return "invalid address alignment";
		case BUS_ADRERR:	return "nonexistent physical address";
		case BUS_OBJERR:	return "object-specific hardware error";
		case BUS_MCEERR_AR:	return "hardware memory error consumed on a machine check; action required";
		case BUS_MCEERR_AO:	return "hardware memory error detected in process but not consumed; action optional";
		}
		break;
	case SIGTRAP:
		switch (code) {
		case TRAP_BRKPT:	return "process breakpoint";
		case TRAP_TRACE:	return "process trace trap";
		}
		break;
		// not handling SIGCHLD, SIGIO, SIGPOLL
	}

	return "?";
}

static void Sys_BacktraceSymbol(FILE *f, void *p) {
	// this doesn't know about shared objects dynamically loaded after
	// fork() in the main process, but it's enough
	char **sym = backtrace_symbols(&p, 1);
	if (sym) {
		fputs(*sym, f);
	} else {
		fprintf(f, "[%p]", p);
	}
	free(sym);
}

static Q_NORETURN void Sys_CrashLogger(int fd, int argc, char *argv[]) {
	siginfo_t	info;
	mcontext_t	mcontext;

	if (!Sys_CrashRead(fd, &info, sizeof(info))) {
		_exit(EXIT_SUCCESS);
	}

	FILE		*f;
	time_t		rawtime;
	char		timeStr[32];

	time(&rawtime);
	strftime(timeStr, sizeof(timeStr), "%Y-%m-%d_%H-%M-%S", localtime(&rawtime)); // or gmtime

	f = fopen(va("crashlog-%s.txt", timeStr), "w");
	if (f == NULL) {
		f = stderr;
	}

	fprintf(f, "---JK2MV Crashlog-----------------------\n");
	fprintf(f, "\n");
	fprintf(f, "Date:               %s", ctime(&rawtime));
	fprintf(f, "Build Version:      " JK2MV_VERSION "\n");
#if defined(PORTABLE)
	fprintf(f, "Build Type:         Portable\n");
#endif
	fprintf(f, "Build Date:         " __DATE__ " " __TIME__ "\n");
	fprintf(f, "Build Arch:         " CPUSTRING "\n");
#ifdef __GNU_LIBRARY__
	fprintf(f, "glibc Version:      %s (%s)\n", gnu_get_libc_version(), gnu_get_libc_release());
#endif
	fprintf(f, "Process:            %s\n", argv[0]);
	fprintf(f, "Process ID:         %d\n", getppid());
	fprintf(f, "Arguments:          ");
	for (int i = 1; i < argc; i++) {
		fprintf(f, "%s ", argv[i]);
	}
	fprintf(f, "\n");

	fprintf(f, "\n");
	fprintf(f, "---Signal Details-----------------------\n");
	fprintf(f, "\n");

	fprintf(f, "Signal:             %d (%s)\n", info.si_signo, strsignal(info.si_signo));
	fprintf(f, "Signal Code:        %d (%s)\n", info.si_code,
			Sys_DescribeSignalCode(info.si_signo, info.si_code));
	switch (info.si_signo) {
	case SIGILL: case SIGFPE: case SIGSEGV: case SIGBUS: case SIGTRAP:
		fprintf(f, "Fault Address:      ");
		Sys_BacktraceSymbol(f, info.si_addr);
		fprintf(f, "\n");
	}

	fprintf(f, "\n");
	fprintf(f, "---Machine Context----------------------\n");
	fprintf(f, "\n");

	if (!Sys_CrashRead(fd, &mcontext, sizeof(mcontext))) {
		goto exit_failure;
	}

#define GREG(X) (mcontext.gregs[REG_ ## X])

#if defined(__amd64__)
	struct selectors_s {
		unsigned short cs, gs, fs, ss;
	};
	struct selectors_s *s;

	s = (struct selectors_s *)&mcontext.gregs[REG_CSGSFS];

	fprintf(f, "   RAX: 0x%.16llx   RBX: 0x%.16llx   RCX: 0x%.16llx\n", GREG(RAX), GREG(RBX), GREG(RCX));
	fprintf(f, "   RDX: 0x%.16llx   RBP: 0x%.16llx   RSI: 0x%.16llx\n", GREG(RDX), GREG(RBP), GREG(RSI));
	fprintf(f, "   RDI: 0x%.16llx   RSP: 0x%.16llx   RIP: 0x%.16llx\n", GREG(RDI), GREG(RSP), GREG(RIP));
	fprintf(f, "    R8: 0x%.16llx    R9: 0x%.16llx   R10: 0x%.16llx\n", GREG(R8),  GREG(R9),  GREG(R10));
	fprintf(f, "   R11: 0x%.16llx   R12: 0x%.16llx   R13: 0x%.16llx\n", GREG(R11), GREG(R12), GREG(R13));
	fprintf(f, "   R14: 0x%.16llx   R15: 0x%.16llx\n",                  GREG(R14), GREG(R15));
	fprintf(f, "EFLAGS: 0x%.8llx            CS: 0x%.4x  GS: 0x%.4x    FS: 0x%.4x  SS: 0x%.4x\n",
			GREG(EFL), s->cs, s->gs, s->fs, s->ss);
#elif defined(__i386__)
	fprintf(f, "    EAX: 0x%.8x    EBX: 0x%.8x    ECX: 0x%.8x    EDX: 0x%.8x\n", GREG(EAX), GREG(EBX), GREG(ECX), GREG(EDX));
	fprintf(f, "    EBP: 0x%.8x    ESI: 0x%.8x    EDI: 0x%.8x    ESP: 0x%.8x\n", GREG(EBP), GREG(ESI), GREG(EDI), GREG(ESP));
	fprintf(f, "     CS: 0x%.4x         DS: 0x%.4x         ES: 0x%.4x         FS: 0x%.4x\n", GREG(CS), GREG(DS), GREG(ES), GREG(FS));
	fprintf(f, " EFLAGS: 0x%.8x    EIP: 0x%.8x     GS: 0x%.4x         SS: 0x%.4x\n", GREG(EFL), GREG(EIP), GREG(GS), GREG(SS));
#endif

	fprintf(f, "\n");
	fprintf(f, "---Console Log--------------------------\n");
	fprintf(f, "\n");

	if (!Sys_CrashRead(fd, &consoleLog, sizeof(consoleLog))) {
		goto exit_failure;
	}

	ConsoleLogWriteOut(f);

	fclose(f);
	_exit(EXIT_SUCCESS);
exit_failure:
	fclose(f);
	_exit(EXIT_FAILURE);
}
