#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <signal.h>
#include <wait.h>
#include <dlfcn.h>
#include "../utils.h"
#include "../ptrace.h"

#define INTEL_RET_INSTRUCTION 0xc3
#define INTEL_INT3_INSTRUCTION 0xcc

// this is the code that will actually be injected into the target process.
// this code is responsible for loading the shared library into the target
// process' address space.  first, it calls malloc() to allocate a buffer to
// hold the filename of the library to be loaded, then calls
// __libc_dlopen_mode(), libc's implementation of dlopen(), to load the desired
// shared library. finally, it calls free() to free the buffer containing the
// library name, and then it breaks into the debugger with an "int $3"
// instruction.
void injectSharedLibrary(long mallocaddr, long freeaddr, long dlopenaddr)
{
	// we're relying heavily on the x64 calling convention to make this work.
	// here are the assumptions I'm making about what data will be located where
	// when the target ends up calling this function:
	//
	//   rdi = address of malloc() in target process
	//   rsi = address of free() in target process
	//   rdx = address of __libc_dlopen_mode() in target process
	//   rcx = size of the path to the shared library we want to load

	// call malloc() from within the target process

	asm(
		// rsi is going to contain the address of free(). it's going to get wiped
		// out by the call to malloc(), so save it on the stack for later
		"push %rsi \n"
		// same thing for rdx, which will contain the address of _dl_open()
		"push %rdx \n"
		// save previous value of r9, because we're going to use it to call malloc() with
		"push %r9 \n"
		// now move the address of malloc() into r9
		"mov %rdi,%r9 \n"
		// choose the amount of memory to allocate with malloc() based on the size
		// of the path to the shared library passed via ecx
		"mov %rcx,%rdi \n"
		// now call r9 in order to call malloc()
		"callq *%r9 \n"
		// after returning from malloc(), pop the previous value of r9 off the stack
		"pop %r9 \n"
		// break in so that we can see what malloc() returned
		"int $3"
	);

	// now call __libc_dlopen_mode()

	asm(
		// get the address of __libc_dlopen_mode() off of the stack so we can call it
		"pop %rdx \n"
		// as before, save the previous value of r9 on the stack
		"push %r9 \n"
		// copy the address of __libc_dlopen_mode() into r9
		"mov %rdx,%r9 \n"
		// the address of the buffer returned by malloc() is going to be the first argument to dlopen
		"mov %rax,%rdi \n"
		// set dlopen's flag argument to 1, aka RTLD_LAZY
		"movabs $1,%rsi \n"
		// now call dlopen
		"callq *%r9 \n"
		// restore old r9 value
		"pop %r9 \n"
		// break in so that we can see what dlopen returned
		"int $3"
	);

	// now call free(). I found that if you put nonzero values in r9,
	// free() assumes they are memory addresses and actually tries to free
	// them, so I apparently have to call it using a register that's not
	// used as part of the x64 calling convention. I chose rbx.

	asm(
		// at this point, rax should still contain our malloc()d buffer from earlier.
		// we're going to free() it, so move rax into rdi to make it the first argument to free().
		"mov %rax,%rdi \n"
		//pop rsi so that we can get the address to free(), which we pushed onto the stack a while ago.
		"pop %rsi \n"
		// save previous rbx value
		"push %rbx \n"
		// load the address of free() into rbx
		"mov %rsi,%rbx \n"
		// zero out rsi, because free() might think that it contains something that should be freed
		"xor %rsi,%rsi \n"
		// break in so that we can check out the arguments right before making the call
		"int $3 \n"
		// call free()
		"callq *%rbx \n"
		// restore previous rbx value
		"pop %rbx"
	);
}

// this function's only purpose in life is to be contiguous to injectSharedLibrary(),
// so that we can use it to more precisely figure out how long injectSharedLibrary() is
void injectSharedLibrary_end()
{
}

// starting at an address somewhere after the end of a function, search for the
// "ret" instruction that ends it. this should be safe, because function
// addresses are word-aligned and padded with "nop"s, so we'll basically search
// through a bunch of "nop"s before finding our "ret". in other words, there's
// no chance we'll run into a 0xc3 byte that corresponds to anything other than
// an actual RET instruction.
unsigned char* findRet(void* endAddr)
{
	unsigned char* retInstAddr = endAddr;
	while(*retInstAddr != INTEL_RET_INSTRUCTION)
	{
		retInstAddr--;
	}
	return retInstAddr;
}

// find the address of the given function within our currently-loaded libc
long getFunctionAddress(char* funcName)
{
	void* self = dlopen("libc.so.6", RTLD_NOLOAD);
	void* funcAddr = dlsym(self, funcName);
	return (long)funcAddr;
}

// restore backed up data and regs and let the target go on its merry way
void restoreStateAndDetach(pid_t target, unsigned long addr, void* backup, int datasize, struct user_regs_struct oldregs)
{
	ptrace_write(target, addr, backup, datasize);
	ptrace_setregs(target, &oldregs);
	ptrace_detach(target);
}

int main(int argc, char** argv)
{
	if(argc < 3)
	{
		printf("usage: %s [process-name] [library-to-inject]\n", argv[0]);
		return 1;
	}

	char* processName = argv[1];
	char* libname = argv[2];
	char* libPath = realpath(libname, NULL);

	if(!libPath)
	{
		fprintf(stderr, "can't find file \"%s\"\n", libname);
		return 1;
	}

	int libPathLength = strlen(libPath) + 1;

	int mypid = getpid();
	long mylibcaddr = getlibcaddr(mypid);

	// find the addresses of the syscalls that we'd like to use inside the
	// target, as loaded inside THIS process (i.e. NOT the target process)
	long mallocAddr = getFunctionAddress("malloc");
	long freeAddr = getFunctionAddress("free");
	long dlopenAddr = getFunctionAddress("__libc_dlopen_mode");

	// use the base address of libc to calculate offsets for the syscalls
	// we want to use
	long mallocOffset = mallocAddr - mylibcaddr;
	long freeOffset = freeAddr - mylibcaddr;
	long dlopenOffset = dlopenAddr - mylibcaddr;

	pid_t target = findProcessByName(processName);
	if(target == -1)
	{
		fprintf(stderr, "doesn't look like a process named \"%s\" is running right now\n", processName);
		return 1;
	}

	printf("found process \"%s\" with pid %d\n", processName, target);

	// get the target process' libc address and use it to find the
	// addresses of the syscalls we want to use inside the target process
	long targetLibcAddr = getlibcaddr(target);
	long targetMallocAddr = targetLibcAddr + mallocOffset;
	long targetFreeAddr = targetLibcAddr + freeOffset;
	long targetDlopenAddr = targetLibcAddr + dlopenOffset;

	struct user_regs_struct oldregs, regs;
	memset(&oldregs, 0, sizeof(struct user_regs_struct));
	memset(&regs, 0, sizeof(struct user_regs_struct));

	ptrace_attach(target);

	ptrace_getregs(target, &oldregs);
	memcpy(&regs, &oldregs, sizeof(struct user_regs_struct));

	// find a good address to copy code to
	long addr = freespaceaddr(target) + sizeof(long);

	// now that we have an address to copy code to, set the target's rip to
	// it.
	//
	// we have to advance by 2 bytes here for some reason. I have a feeling
	// this is because rip gets incremented by the size of the current
	// instruction, and the instruction at the start of the function to
	// inject always happens to be 2 bytes long, but I never looked into it
	// further.
	regs.rip = addr + 2;

	// pass arguments to my function injectSharedLibrary() by loading them
	// into the right registers. note that this will definitely only work
	// on x64, because it relies on the x64 calling convention, in which
	// arguments are passed via registers rdi, rsi, rdx, rcx, r8, and r9.
	// see comments in injectSharedLibrary() for more details.
	regs.rdi = targetMallocAddr;
	regs.rsi = targetFreeAddr;
	regs.rdx = targetDlopenAddr;
	regs.rcx = libPathLength;

	ptrace_setregs(target, &regs);
	//printf("setting target regs to:\n");
	//printregs(target);

	// figure out the size of injectSharedLibrary() so we know how big of a buffer to allocate. 

	int injectSharedLibrary_size = (int)injectSharedLibrary_end - (int)injectSharedLibrary;

	// also figure out where the RET instruction at the end of
	// injectSharedLibrary() lies so that we can overwrite it with an INT 3
	// in order to break back into the target process. note that on x64,
	// gcc and clang both force function addresses to be word-aligned,
	// which means that functions are padded with NOPs. as a result, even
	// though we've found the length of the function, it is very likely
	// padded with NOPs, so we need to actually search to find the RET.
	int injectSharedLibrary_ret = (int)findRet(injectSharedLibrary_end) - (int)injectSharedLibrary;

	// back up whatever data used to be at the address we want to modify.
	char* backup = malloc(injectSharedLibrary_size * sizeof(char));
	ptrace_read(target, addr, backup, injectSharedLibrary_size);

	// set up a buffer to hold the code we're going to inject into the
	// target process.
	char* newcode = malloc(injectSharedLibrary_size * sizeof(char));
	memset(newcode, 0, injectSharedLibrary_size * sizeof(char));

	// copy the code of injectSharedLibrary() to a buffer.
	memcpy(newcode, injectSharedLibrary, injectSharedLibrary_size - 1);
	// overwrite the RET instruction with an INT 3.
	newcode[injectSharedLibrary_ret] = INTEL_INT3_INSTRUCTION;

	// copy injectSharedLibrary()'s code to the target address inside the
	// target process' address space.
	ptrace_write(target, addr, newcode, injectSharedLibrary_size);

	// now that the new code is in place, let the target run our injected
	// code.
	ptrace_cont(target);

	// at this point, the target should have run malloc(). check its return
	// value to see if it succeeded, and bail out cleanly if it didn't.
	struct user_regs_struct malloc_regs;
	memset(&malloc_regs, 0, sizeof(struct user_regs_struct));
	ptrace_getregs(target, &malloc_regs);
	unsigned long long targetBuf = malloc_regs.rax;
	if(targetBuf == 0)
	{
		fprintf(stderr, "malloc() failed to allocate memory\n");
		restoreStateAndDetach(target, addr, backup, injectSharedLibrary_size, oldregs);
		free(backup);
		free(newcode);
		return 1;
	}

	// if we get here, then malloc likely succeeded, so now we need to copy
	// the path to the shared library we want to inject into the buffer
	// that the target process just malloc'd. this is needed so that it can
	// be passed as an argument to dlopen later on.

	// read the current value of rax, which contains malloc's return value,
	// and copy the name of our shared library to that address inside the
	// target process.
	ptrace_write(target, targetBuf, libPath, libPathLength);

	// continue the target's execution again in order to call dlopen.
	ptrace_cont(target);

	// check out what the registers look like after calling dlopen. 
	struct user_regs_struct dlopen_regs;
	memset(&dlopen_regs, 0, sizeof(struct user_regs_struct));
	ptrace_getregs(target, &dlopen_regs);
	unsigned long long libAddr = dlopen_regs.rax;

	// if rax is 0 here, then dlopen failed, and we should bail out cleanly.
	if(libAddr == 0)
	{
		fprintf(stderr, "__libc_dlopen_mode() failed to load %s\n", libname);
		restoreStateAndDetach(target, addr, backup, injectSharedLibrary_size, oldregs);
		free(backup);
		free(newcode);
		return 1;
	}

	// if rax is nonzero, then our library was successfully injected.
	printf("library \"%s\" successfully injected\n", libname);

	// as a courtesy, free the buffer that we allocated inside the target
	// process. we don't really care whether this succeeds, so don't
	// bother checking the return value.
	ptrace_cont(target);

	// at this point, if everything went according to plan, we've loaded
	// the shared library inside the target process, so we're done. restore
	// the old state and detach from the target.
	restoreStateAndDetach(target, addr, backup, injectSharedLibrary_size, oldregs);
	free(backup);
	free(newcode);

	return 0;
}
