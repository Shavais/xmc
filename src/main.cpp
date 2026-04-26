#include "pch/pch.h"

#include "CommandLineReader.h"
#include "ProjectFileReader.h"
#include "Builder.h"
#include "Tester.h"

#include "tool/Logger.h"

using namespace xmc;

int main(int argc, char* argv[])
{
	try
	{
		InitializeLogging();
		auto args = CommandLineReader::Parse(argc, argv);
		auto project = ProjectFileReader::Load(args.ProjectName, args.ConfigName);
		Builder::Build(args, project);
		if (args.Test)  Tester::RunTests(args, project);
		else if (args.Suite) Tester::RunSuite(args, project);
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << "error: " << e.what() << "\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "unhandled exception: " << e.what() << "\n";
	}
	catch (...)
	{
		std::cerr << "an unknown exception was thrown\n";
	}

	if (Logger::ErrorOccurred) return -1;
	return 0;
}