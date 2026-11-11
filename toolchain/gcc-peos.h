/* GCC's target header for PeOS (x86_64-peos): the d2decomped port, phase 2.
   tools/peos-toolchain/patch.py copies it to gcc/config/peos.h.

   A program for PeOS links statically against libpeos (the native base)
   and libpeos-compat (what ports need: pthread, dirent, the rest of a C
   library libstdc++ asks for). The compat crt0 sets the first thread's TLS,
   runs the constructors (.init_array: crtbegin's frame_dummy registers the
   exception tables there), main, then the atexit list and destructors. */

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()          \
  do {                                    \
      builtin_define ("__peos__");        \
      builtin_assert ("system=peos");     \
  } while (0)

#undef STARTFILE_SPEC
#define STARTFILE_SPEC "crt0.o%s crtbegin.o%s"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "crtend.o%s"

#undef LIB_SPEC
#define LIB_SPEC "--start-group -lpeos-compat -lpeos --end-group"

/* Static only, pages of 4 KiB (as PeOS's loader maps them), no executable
   stack, the exception table header for the unwinder. */
#undef LINK_SPEC
#define LINK_SPEC "-static -z max-page-size=0x1000 -z noexecstack --eh-frame-hdr"

/* Without shared libgcc the unwinder is inside libgcc.a itself. */
#undef LIBGCC_SPEC
#define LIBGCC_SPEC "-lgcc"
