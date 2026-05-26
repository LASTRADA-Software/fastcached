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

std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string const& /*pidfile*/)
{
    return nullptr; // unsupported on Windows
}

#else

namespace
{

    class PosixDaemonHost final: public IDaemonHost
    {
      public:
        explicit PosixDaemonHost(std::string pidfile) noexcept:
            _pidfile { std::move(pidfile) }
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
            std::ignore = ::chdir("/");

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

            // Write pidfile (best effort).
            if (!_pidfile.empty())
            {
                if (std::ofstream out { _pidfile, std::ios::trunc }; out)
                    out << std::format("{}\n", ::getpid());
            }

            if (!body)
                return 0;
            return body();
        }

      private:
        std::string _pidfile;
    };

} // namespace

std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string const& pidfile)
{
    return std::make_unique<PosixDaemonHost>(pidfile);
}

#endif // !_WIN32

} // namespace FastCache
