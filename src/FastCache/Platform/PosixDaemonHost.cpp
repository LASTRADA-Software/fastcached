// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/IDaemonHost.hpp>

#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#if !defined(_WIN32)
    #include <sys/stat.h>
    #include <sys/types.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace FastCache
{

#if defined(_WIN32)

std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string const& /*pidfile*/, std::string const& /*workingDirectory*/)
{
    return nullptr; // unsupported on Windows
}

#else

namespace
{

    class PosixDaemonHost final: public IDaemonHost
    {
      public:
        PosixDaemonHost(std::string pidfile, std::string workingDirectory) noexcept:
            _pidfile { std::move(pidfile) },
            _workingDirectory { std::move(workingDirectory) }
        {
        }

        int Run(Body body) override
        {
            // First fork: detach from parent.
            auto firstPid = ::fork();
            if (firstPid < 0)
                return EXIT_FAILURE;
            if (firstPid > 0)
                std::exit(EXIT_SUCCESS);

            ::setsid();

            // Second fork: ensure no controlling terminal can be acquired.
            auto secondPid = ::fork();
            if (secondPid < 0)
                return EXIT_FAILURE;
            if (secondPid > 0)
                std::exit(EXIT_SUCCESS);

            ::umask(0);

            // Redirect stdio to /dev/null.
            ::close(STDIN_FILENO);
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
            auto fdNull = ::open("/dev/null", O_RDWR);
            if (fdNull >= 0)
            {
                ::dup2(fdNull, STDIN_FILENO);
                ::dup2(fdNull, STDOUT_FILENO);
                ::dup2(fdNull, STDERR_FILENO);
                if (fdNull > STDERR_FILENO)
                    ::close(fdNull);
            }

            // Write pidfile (best effort), BEFORE the chdir below. Where a relative
            // pidfile lands must not depend on which directory this host was told to
            // move to -- otherwise giving the compile node a directory of its own
            // silently relocates its pidfile, which is the same class of defect as
            // the one that change is fixing. It resolves against the directory the
            // operator ran the command in, for both binaries; it used to resolve
            // against `/`, where only root can write.
            if (!_pidfile.empty())
            {
                if (std::ofstream out { _pidfile, std::ios::trunc }; out)
                    out << std::format("{}\n", ::getpid());
            }

            // **The caller's directory, never a constant.** See `MakePosixDaemonHost`:
            // for the compile node `/` turns a `-fdebug-prefix-map` rule into a
            // rewriter of every absolute path in the object it produces (#784).
            //
            // `/` on failure rather than staying put: this host has already detached,
            // so the alternative is a daemon pinned to whatever directory the operator
            // happened to be in, which is the busy-mount-point problem daemonizing
            // exists to avoid. It is also the value this call had before, so a host
            // whose directory cannot be reached is exactly as it was.
            if (::chdir(_workingDirectory.c_str()) != 0)
                std::ignore = ::chdir("/");

            if (!body)
                return 0;
            return body();
        }

      private:
        std::string _pidfile;
        std::string _workingDirectory;
    };

} // namespace

std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string const& pidfile, std::string const& workingDirectory)
{
    return std::make_unique<PosixDaemonHost>(pidfile, workingDirectory);
}

#endif // !_WIN32

} // namespace FastCache
