# Repository Guidelines

## Project Structure

- `src/main.cpp` starts the Drogon server. Put startup and service wiring in `src/app/server/` and HTTP handlers in its `routes/` subdirectory.
- Define all settings, defaults, and field validation in `src/app/config.h`. Use public members on a plain config struct and call `load_config<Config>(root)` at startup. The member names supply the JSON keys and environment variable names. See the [README configuration example](README.md#configuration).
- Keep business rules, transaction orchestration, and upstream clients in `src/app/services/`. Keep domain DAOs and SQL in `src/app/dao/`. Put persisted records and DTOs in separate model headers under `src/app/models/`.
- `src/framework/` holds reusable database, CRUD, HTTP, JWT, storage, and support code. The `template_core` target builds this layer independently. Framework code must not include app headers.
- Include `framework/db/base_dao.h` to define a DAO. Its implementation headers are private to the framework.
- Keep each C++ header beside its implementation and use paths relative to `src/` when including headers from another directory.
- `views/` holds Drogon CSP templates compiled by Xmake. `public/` holds JavaScript, Tailwind source (`tailwind.css`), generated stylesheet (`demo.css`), and runtime uploads. `tests/` contains the Catch2 suite.
- Xmake writes compiled output to `build/` and keeps local build configuration in `.xmake/`.

## Build and Run

Run commands from the project root with Xmake and a C++26 compiler and standard library that support `std::println`.

- `xmake f -m debug` or `xmake f -m release` selects the build mode.
- Set a custom C++ namespace with `xmake f --app_namespace=MyApp`. The default is `App`.
- `xmake` resolves dependencies and builds the server. First-time dependency resolution requires network access or a populated Xmake cache.
- `xmake build template_core` builds the shared library by itself.
- `xmake run app_server` starts the server at `http://127.0.0.1:8080` with the default `config.json`. The shipped config sets the local demo credentials with `test_user` and `test_password`. When both are omitted, the server generates demo credentials on first launch and prints them. Startup applies configured credentials to the demo account in `.local/`. Use the shipped credentials only for local testing.
- After editing the page or Tailwind source, run `npm ci` and `npm run build:css`. Commit the regenerated `public/demo.css`.

The optional `.env` supplies `APP_JWT_SECRET`. See `.env.example` for the format. An existing environment variable takes precedence. Without a configured key, a server restart invalidates previously issued tokens. Keep `.env`, database files, and uploaded content out of commits.

Config fields resolve from the process environment, then `.env`, then `config.json`. Use `APP_` followed by the uppercase JSON key, such as `APP_PORT` or `APP_LOG_LEVEL`. Put sensitive values in the environment or ignored `.env` and omit their keys from `config.json`. Validate the combined configuration.

## Coding Style

Use four spaces and place opening braces on the declaration line. Use `.h` for C++ headers and `.cpp` for implementations. Keep route handlers small and delegate persistence and outbound calls to their modules. Always use `co_await` for asynchronous application code and `std::println` for console output, including logs. Use `api_ok` and `api_error` for typed API responses.

Use `snake_case` for project-owned C++ functions, methods, variables, fields, constants, and enum values. Use `PascalCase` for types. Keep macro names and names required by third-party APIs in their existing spelling. Preserve existing JSON keys, configuration keys, and stored database names when renaming C++ identifiers.

Prefer values for direct ownership and references for required borrows. Use `std::optional<std::reference_wrapper<T>>` for optional borrows and smart pointers for explicit heap ownership. Avoid raw pointers in application interfaces. Adapt third-party APIs that require raw pointers at the integration boundary.

## Database and Service Layers

- Define persistent fields in a snake_case aggregate with a static `table_name`. Wrap it in `DaoRecord<Fields>` and derive the entity DAO from `BaseDao<Derived, Record>`. Use field annotations for keys, constraints, indexes, defaults, and foreign references. See the [README database example](README.md#database-example).
- Open the database backend once at startup and pass it to each DAO's async `init(database)`. Initialize parent DAOs before DAOs that reference them. Use the base DAO's coroutine CRUD methods and typed filters for ordinary queries. Keep custom SQL in the DAO, and bind runtime values through `execute_sql`.
- Build list queries with `dao.find()` or `dao.find(tx)` and execute them with `co_await … .collect()`. The default is 100 rows at offset 0. Set a different window with `.limit()` and `.offset()`. Use `.where<Member>(value)` for a typed filter, `.where(predicate)` for a composed predicate, and `.matching(filters)` for runtime `EqualityFilters`. Add projection, sorting, and foreign loading with `.select()`, `.select_all()`, `.order_by()`, and `.with_foreign()`. List queries exclude soft-deleted rows unless you add `.include_deleted()`.
- Finish ID lookups and counts with `.collect()` too. Use `find_by_id(id)` for an optional full record and `count()` for the total across all matching active rows. Pass a transaction as the first argument when needed. Both builders support `.include_deleted()`.
- Prefer named filters such as `.where_in<Member>(ids)` and `.where_gte<Member>(value)` with `find()` and `count()`. Use `.where(predicate)` for reusable predicates, negation, and tuple cursors.
- Use `update_by_id(id).set<Member>(value).execute()` for partial updates and `insert_if_absent<Key>(record)` or `upsert<Key, Members...>(record)` for declared single-column conflict targets. Keep the DAO and any borrowed transaction alive until the awaited operation completes.
- Use `create_many(records)` and `update_many(records)` for atomic batches. Pass the existing transaction first to scope the batch to a savepoint. Create returns keys in input order. Update returns false and rolls back the batch if any target is missing or soft-deleted.
- Use `delete_many(ids)` for hard deletes and `soft_delete_many(ids)` for soft deletes. Both use the same batch transaction rules and return the number of rows changed.
- Give each service its initialized DAO by value and keep business logic in the service. Register services in the server's `Services` container. Route handlers should validate requests, `co_await` service methods, and build responses. Keep database queries and persistence records inside the service and DAO layers.

```cpp
auto posts = co_await post_dao.find()
    .where<&SocialPost::author_id>(user_id)
    .select<&SocialPost::created_at>()
    .order_by<&SocialPost::created_at>(SortDirection::desc)
    .limit(20)
    .collect();
```

## Testing

- Run the Catch2 suite with `xmake test --output-on-failure`. Name test files `*_tests.cpp` and describe the observable behavior in each test case name.
- Give each test case one behavior or a closely related scenario. Use separate cases for successful operations, expected errors, and edge conditions. Keep related assertions together when they verify the same outcome.
- Keep cases independent of execution order. Share fixture setup where it reduces duplication, and give each case isolated mutable state, such as its own in-memory SQLite database or temporary directory.
- Use loopback fixtures for HTTP integration tests and upstream calls. Smoke-test changes to the demo page and verify returned download links when applicable.

## Commits and Pull Requests

Use a short imperative subject with a prefix such as `feat: add upload route`. Summarize the change, report verification results, and link a related issue when one exists.
