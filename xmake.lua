set_project("hello")
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

add_requires("drogon v1.9.12", { configs = { cpp20 = true } })
add_requires("cpr 1.12.0")
if is_plat("macosx") then
	-- Keep libcurl's OpenSSL 3 inside its shared library so Trantor can link OpenSSL 1.1.
	add_requireconfs("cpr.libcurl", { configs = { shared = true } })
	add_requireconfs("cpr.libcurl.openssl3", { private = true })
end
add_requires("dotenv-cpp 9275210b8abf1a551fb81e0ba45866286d764acd", "glaze v7.0.2", "jwt-cpp 0.7.2")
add_requires("simdjson", "sqlite3", "libsodium", "catch2 3.x")

target("hello_core")
set_kind("static")
add_files("src/*.cpp|main.cpp")
add_includedirs("src", { public = true })
add_packages("drogon", "cpr", "dotenv-cpp", "glaze", "jwt-cpp", "simdjson", "sqlite3", "libsodium", { public = true })

target("hello")
set_kind("binary")
add_files("src/main.cpp")
add_deps("hello_core")
set_rundir("$(projectdir)")

target("hello_tests")
set_kind("binary")
set_default(false)
add_files("tests/*.cpp")
add_deps("hello_core")
add_packages("catch2", { components = { "main", "lib" } })
add_tests("suite", { run_timeout = 30000 })
