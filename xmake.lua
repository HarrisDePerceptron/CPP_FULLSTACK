set_project("app_template")
set_languages("cxx26")
add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

package("dotenv-cpp")
set_homepage("https://github.com/laserpants/dotenv-cpp")
set_license("BSD-3-Clause")
set_kind("library", { headeronly = true })
add_urls("https://github.com/laserpants/dotenv-cpp.git")
on_install(function(package)
	local headers = package:installdir("include", "laserpants", "dotenv")
	local licenses = package:installdir("share", "licenses", "dotenv-cpp")
	os.mkdir(headers, licenses)
	os.cp("include/laserpants/dotenv/dotenv.h", headers)
	os.cp("LICENSE", licenses)
end)
package_end()

if is_host("macosx") then
	-- Autoconf cannot use the older m4 shipped with macOS.
	add_requireconfs("**.m4", { version = ">=1.4.16", override = true })
end

add_requires("drogon v1.9.12", { configs = { cpp20 = true, sqlite3 = true } })
add_requires("dotenv-cpp 9275210b8abf1a551fb81e0ba45866286d764acd", "glaze v7.0.2", "jwt-cpp 0.7.2")
add_requires("magic_enum v0.9.8")
add_requires("simdjson", "libsodium", "catch2 3.x")

option("app_namespace")
set_default("App")
set_showmenu(true)
set_description("C++ namespace for the application")
option_end()

target("template_core")
on_config(function(target)
	local name = get_config("app_namespace")
	assert(
		type(name) == "string" and name:match("^[A-Za-z_][A-Za-z0-9_]*$"),
		"app_namespace must be a single C++ identifier"
	)
	assert(
		target:check_cxxsnippets({ namespace_name = "namespace " .. name .. " {}" }),
		"app_namespace must be a valid C++ namespace identifier"
	)
end)
set_kind("static")
add_options("app_namespace")
add_defines("APP_NAMESPACE=$(app_namespace)", { public = true })
add_files("src/framework/**.cpp")
add_includedirs("src", { public = true })
add_packages(
	"drogon",
	"glaze",
	"jwt-cpp",
	"libsodium",
	"magic_enum",
	{ public = true }
)

target("app_core")
set_kind("static")
add_files("src/app/**.cpp")
add_deps("template_core", { public = true })
add_packages("dotenv-cpp", "simdjson", { public = true })

rule("drogon.view")
set_extensions(".csp")
before_buildcmd_file(function(target, batchcmds, sourcefile, opt)
	local ctl = path.join(target:pkg("drogon"):installdir(), "bin", "drogon_ctl")
	local output = path.join(target:autogendir(), "views")
	local generated = path.join(output, "AppViews_" .. path.basename(sourcefile) .. ".cc")
	local object = target:objectfile(generated)
	table.insert(target:objectfiles(), object)

	batchcmds:show_progress(opt.progress, "compiling view %s", sourcefile)
	batchcmds:mkdir(output)
	batchcmds:vrunv(ctl, { "create", "view", path(sourcefile), "-o", path(output), "-n", "AppViews" })
	batchcmds:compile(generated, object)
	batchcmds:add_depfiles(sourcefile, ctl)
	batchcmds:set_depmtime(os.mtime(object))
	batchcmds:set_depcache(target:dependfile(object))
end)
rule_end()

target("app_server")
set_kind("binary")
add_files("src/main.cpp")
add_rules("drogon.view")
add_packages("drogon")
add_files("views/*.csp")
add_deps("app_core")
set_rundir("$(projectdir)")

target("app_tests")
set_kind("binary")
set_default(false)
add_files("tests/*.cpp")
add_deps("app_core")
add_packages("catch2", { components = { "main", "lib" } })
add_tests("suite", { run_timeout = 30000 })
