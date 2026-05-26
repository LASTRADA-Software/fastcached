// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/IDaemonHost.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#if !defined(_WIN32)
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace FastCache
{

#if defined(_WIN32)

std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string /*pidfile*/)
{
    return nullptr; // unsupported on Windows
}

#else

namespace
{

    class PosixDaemonHost final: public IDaemonHost
    {
      public:
        explicit PosixDaemonHost(std::string pidfile) noexcept: _pidfile { std::move(pidfile) } {}

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
            (void) ::chdir("/");

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
                auto* fp = std::fopen(_pidfile.c_str(), "w");
                if (fp)
                {
                    std::fprintf(fp, "%d\n", ::getpid());
                    std::fclose(fp);
                }
            }

            if (!body)
                return 0;
            return body();
        }

      private:
        std::string _pidfile;
    };

} // namespace

std::unique_ptr<IDaemonHost> MakePosixDaemonHost(std::string pidfile)
{
    return std::make_unique<PosixDaemonHost>(std::move(pidfile));
}

#endif // !_WIN32

} // namespace FastCache
