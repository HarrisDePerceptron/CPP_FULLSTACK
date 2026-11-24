# Repository Guidelines

## Project Structure

- `src/main.cpp` contains the entry point for the `hello` C++ executable.
- `xmake.lua` defines the executable and its debug and release build modes. The target automatically includes `src/*.cpp`. Update the source pattern if you introduce nested source directories.
- `build/` contains generated build output, and `.xmake/` stores local Xmake configuration and cache data. Both are ignored by Git.
- The project currently has no dedicated test or asset directories.

## Build and Run

Run these commands from the project root with Xmake and a C++ toolchain installed.

- `xmake` builds the executable with the active configuration.
- `xmake run hello` runs the executable.
- `xmake f -m debug` selects debug mode. Run `xmake` again to build, then use `xmake run -d hello` to launch the debugger.
- `xmake f -m release` selects release mode. Run `xmake` again to build.

## Coding Style

Match the existing C++ style in `src/main.cpp`. Use four spaces per indentation level, place the opening brace on the function declaration line, and use `.cpp` for implementation files. Keep standard-library includes at the top. No formatter or linter is configured.

## Testing

The project has no automated test framework or coverage threshold. After changing the program or build configuration, run `xmake` and `xmake run hello`. The current smoke check is that the executable prints `hello world!` on one line and exits successfully. If you change that behavior intentionally, verify the new output and exit status.

## Commits and Pull Requests

Use a short, imperative commit subject, such as `Add command-line greeting`. In the pull request, summarize the behavior change, report the commands you ran and their results, and link a related issue if one exists.
