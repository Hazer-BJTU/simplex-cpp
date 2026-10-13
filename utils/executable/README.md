# Executable location

`executable_iface` provides `executable::location(std::error_code&)` for locating
installed assets beside the application. It is shared by the model dispatcher
and the subprocess platform helper without a model-to-process dependency.

On Linux, an absolute main-program name from `dl_iterate_phdr` is canonicalized.
This handles explicit musl loader invocation, where `/proc/self/exe` names the
loader. The portable launcher always passes an absolute application path.
Unnamed or relative program-header entries use Boost.DLL's native fallback;
other platforms use that fallback directly. The helper does not trust an
environment-supplied installation directory. Error-code calls return an empty
path on lookup failure; allocation failures can still throw.

`test_executable_location` checks lookup after a working-directory change.
The musl packaging validation additionally invokes it through the loader.
