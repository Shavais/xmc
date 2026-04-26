#ifndef SV_LOGGER
#define SV_LOGGER
#define FORCE_DEBUG_OUTPUT 

#include <atomic>
#include <iosfwd>
#include <ostream>
#include <streambuf>
#include <string>
#include <mutex> // Added for thread safety

extern bool StopLogger;
void InitializeLogging();

class Logger : public std::streambuf {
public:
	Logger(std::ofstream* out, bool isErrorStream = false);
	~Logger();
	static const std::string Init();

	inline static std::atomic_bool ErrorOccurred = false;

private:
	virtual std::streamsize xsputn(const char_type* s, std::streamsize n) override;
	virtual int_type overflow(int_type c) override;
	virtual int sync() override;

	// Added helper for atomic line writes
	void CommitLine(const std::string& line);

private:
	std::ofstream* output_;
	bool isError_;
	static std::mutex writeMutex; // Protects shared output
};

extern std::ostream osdebug;
extern std::ostream oserror;

std::string GetLastErrorMessage();

#define debug osdebug << __FILE__ << "(" << __LINE__ << "): "
#define errlog oserror << __FILE__ << "(" << __LINE__ << "): "
#endif