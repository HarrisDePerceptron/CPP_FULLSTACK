# Repository Guidelines

## Project Structure

- `src/main.cpp` starts the Drogon server. `src/server.cpp` registers routes and middleware. Neighboring files handle configuration, users, JWTs, uploads, and upstream content.
- `public/` holds the HTML, JavaScript, compiled Tailwind CSS, and runtime uploads. `web/demo.css` is the Tailwind source. `tests/` contains the Catch2 suite.
- Xmake writes compiled output to `build/` and keeps local build configuration in `.xmake/`.

## Build and Run

Run commands from the project root with Xmake and a C++26 compiler and standard library that support `std::println`.

- `xmake f -m debug` or `xmake f -m release` selects the build mode.
- `xmake` resolves dependencies and builds the server. First-time dependency resolution requires network access or a populated Xmake cache.
- `xmake run hello` starts the server at `http://127.0.0.1:8080` with the default `config.json`. Save the demo credentials printed on first launch. SQLite stores the account under `.local/`.
- After editing the page or Tailwind source, run `npm ci` and `npm run build:css`. Commit the regenerated `public/demo.css`.

The optional `.env` supplies `HELLO_JWT_SECRET`. See `.env.example` for the format. An existing environment variable takes precedence. Without a configured key, a server restart invalidates previously issued tokens. Keep `.env`, database files, and uploaded content out of commits.

## Coding Style

Use four spaces and place opening braces on the declaration line. Use `.hpp` for C++ headers and `.cpp` for implementations. Keep route handlers small and delegate persistence and outbound calls to their modules. Always use `co_await` for asynchronous application code and `std::println` for console output, including logs. Use `jsonResponse` for typed API responses.

## Testing

Run `xmake test` to run Catch2. Name test files `*_tests.cpp` and describe observable behavior in test case names. Use temporary state and a loopback fixture for upstream calls. Smoke-test HTTP changes through the demo page and verify returned download links.

## Commits and Pull Requests

Use a short imperative subject with a prefix such as `feat: add upload route`. Summarize the change, report verification results, and link a related issue when one exists.
