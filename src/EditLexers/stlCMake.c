#include "EditLexer.h"
#include "EditStyle.h"

// https://cmake.org/documentation/

static KEYWORDLIST Keywords_CMake = {{
"break() else() elseif() return() continue() "
"foreach() function() if() macro() while() endforeach() endfunction() endif() endmacro() endwhile() "
, // 1 Command
"add_compile_options() "
"add_custom_command() add_custom_target() add_definitions() add_dependencies() add_executable() "
"add_library() add_subdirectory() add_test() aux_source_directory() build_command() "
"cmake_host_system_information() cmake_minimum_required() cmake_parse_arguments() "
"cmake_policy() configure_file() create_test_sourcelist() define_property() enable_language() "
"enable_testing() execute_process() export() file() find_file() find_library() find_package() "
"find_path() find_program() fltk_wrap_ui() get_cmake_property() get_directory_property() "
"get_filename_component() get_property() get_source_file_property() get_target_property() "
"get_test_property() include() include_directories() include_external_msproject() "
"include_regular_expression() install() link_directories() link_libraries() list() load_cache() "
"mark_as_advanced() math() message() option() project() qt_wrap_cpp() qt_wrap_ui() remove_definitions() "
"separate_arguments() set() set_directory_properties() set_property() set_source_files_properties() "
"set_target_properties() set_tests_properties() site_name() source_group() string() "
"target_compile_definitions() target_compile_features() target_compile_options() "
"target_include_directories() target_link_libraries() target_sources() "
"try_compile() try_run() unset() variable_watch()"

"ctest_build() ctest_configure() ctest_coverage() ctest_empty_binary_directory() ctest_memcheck() "
"ctest_read_custom_files() ctest_run_script() ctest_sleep() ctest_start() ctest_submit() "
"ctest_test() ctest_update() ctest_upload() "

, // 1 Variables that Provide Information
NULL
, // 2 Variables That Change Behavior
NULL
, // 3 Variables That Describe the System
NULL
, NULL, NULL, NULL, NULL

, NULL, NULL, NULL, NULL, NULL, NULL, NULL
}};

static EDITSTYLE Styles_CMake[] = {
	EDITSTYLE_DEFAULT,
	{ SCE_CMAKE_WORD, NP2STYLE_Keyword, EDITSTYLE_HOLE(L"Keyword"), L"fore:#0000FF" },
	{ SCE_CMAKE_COMMANDS, NP2STYLE_Command, EDITSTYLE_HOLE(L"Command"), L"fore:#FF8000" },
	{ SCE_CMAKE_COMMENT, NP2STYLE_Comment, EDITSTYLE_HOLE(L"Comment"), L"fore:#608060" },
	{ MULTI_STYLE(SCE_CMAKE_STRINGDQ, SCE_CMAKE_STRINGSQ, SCE_CMAKE_STRINGBT, 0), NP2STYLE_String, EDITSTYLE_HOLE(L"String"), L"fore:#008080" },
	//{ SCE_CMAKE_NUMBER, NP2STYLE_Number, EDITSTYLE_HOLE(L"Number"), L"fore:#FF0000" },
	{ SCE_CMAKE_VARIABLE, NP2STYLE_Variable, EDITSTYLE_HOLE(L"Variable"), L"fore:#CC3300" },
	{ SCE_CMAKE_OPERATOR, NP2STYLE_Operator, EDITSTYLE_HOLE(L"Operator"), L"fore:#B000B0" },
	{ SCE_CMAKE_FUNCATION, NP2STYLE_Function, EDITSTYLE_HOLE(L"Function"), L"fore:#808000" },
	{ SCE_CMAKE_MACRO, NP2STYLE_Macro, EDITSTYLE_HOLE(L"Macro"), L"fore:#808000" },
};

EDITLEXER lexCMake = {
	SCLEX_CMAKE, NP2LEX_CMAKE,
	EDITLEXER_HOLE(L"CMake Script", Styles_CMake),
	L"cmake; ctest",
	&Keywords_CMake,
	Styles_CMake
};

