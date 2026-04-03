#ifndef SV_LOGGER
#define SV_LOGGER

#define FORCE_DEBUG_OUTPUT        // uncomment this to get debug output in the Release configuration

#include <iosfwd>
#include <ostream>
#include <streambuf>
#include <string>

extern bool StopLogger;

void InitializeLogging();

class Logger : public std::streambuf
{
public:
	Logger(std::ofstream* out);
	~Logger();

	static const std::string Init();
		
private:
	virtual std::streamsize xsputn(const char_type* s, std::streamsize n) override;
	virtual int_type overflow(int_type c) override;
	virtual int sync() override;

	std::ofstream* output_;
};

extern std::ostream osdebug;
extern std::ostream oserror;

std::string GetLastErrorMessage();

#define debug osdebug << __FILE__ << "(" << __LINE__ << "): "	
#define errlog oserror << __FILE__ << "(" << __LINE__ << "): "

#endif
