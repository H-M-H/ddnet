#include <string>
#include <vector>

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/compiler/command_line_interface.h>
#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/stubs/logging.h>

#include <inja.hpp>

using namespace google::protobuf;
using namespace google::protobuf::compiler;

class CinjaCodeGenerator final : public google::protobuf::compiler::CodeGenerator
{
	bool Generate(
		const FileDescriptor* File,
		const std::string& Parameter,
		GeneratorContext* GeneratorCtx,
		std::string* error) const override
	{
		std::vector<std::pair<std::string, std::string>> Options;
		ParseGeneratorParameter(Parameter, &Options);

		nlohmann::json Data;

		std::string TemplFile;
		std::string OutFile;
		for (auto O : Options)
		{
			if (O.first == "Out")
				OutFile = O.second;
			if (O.first == "Template")
				TemplFile = O.second;
			Data[O.first] = O.second;
		}
		if (TemplFile.empty())
		{
			*error =
				"The template file to read needs to be specified with --inja_opt=Template=FILE.";
			return false;
		}
		if (OutFile.empty())
		{
			*error = "The file to be written needs to be specified with --inja_opt=Out=FILE.";
			return false;
		}

		Data["Services"] = nlohmann::json::object();

		for (int i = 0; i < File->service_count(); ++i)
		{
			const auto Service = File->service(i);
			for (int j = 0; j < Service->method_count(); ++j)
			{
				const auto Method = File->service(i)->method(j);
				const auto InputType = Method->input_type();
				const auto OutputType = Method->output_type();
				Data["Services"][Service->name()][Method->name()]["In"] = InputType->name();
				Data["Services"][Service->name()][Method->name()]["Out"] = OutputType->name();
			}
		}

		inja::Environment Env;
		inja::Template Templ = Env.parse_template(TemplFile);
		std::string Render = Env.render(Templ, Data);

		std::unique_ptr<io::ZeroCopyOutputStream> Output(GeneratorCtx->Open(OutFile));
		GOOGLE_CHECK(Output.get());
		io::Printer Printer(Output.get(), '$');
		Printer.PrintRaw(Render);

		return true;
	}
};

int main(int Argc, char* Argv[])
{
	google::protobuf::compiler::CommandLineInterface Cli;
	CinjaCodeGenerator Generator;

	Cli.RegisterGenerator(
		"--inja_out", "--inja_opt", &Generator, "Use inja template engine to generate code.");

	return google::protobuf::compiler::PluginMain(Argc, Argv, &Generator);
}
