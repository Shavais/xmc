#include "experiments.h"
#include "tool/Logger.h"

#include <iostream>

int main() 
{
	try
	{
		InitializeLogging();
		CallCppFunction();
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << "error: " << e.what() << std::endl;	
	}
	catch (const std::exception& e)
	{
		std::cerr << "unhandled exception: " << e.what() << std::endl;
	}
	catch (...)
	{
		std::cerr << "an unknown exception was thrown" << std::endl;
	}

	return 0;
}
