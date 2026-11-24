# App Template

A C++26 web application starter built with Drogon and Xmake. The demo includes a server-rendered page, cookie-based sign-in, SQLite user storage, file uploads and downloads, and an upstream JSON endpoint.

## Getting started

Install Xmake for your platform.

**macOS** with Homebrew

```sh
brew install xmake
```

**Linux** with the official installer

```sh
curl -fsSL https://xmake.io/shget.text | bash
```

Open a new shell after installation and run `xmake --version`. For package-manager alternatives, see the [Xmake installation guide](https://xmake.io/guide/quick-start.html#installation).

Use a C++26 compiler and standard library with `std::println` support. The first build needs network access or a populated Xmake package cache. From the project root, run:

```sh
xmake f -m debug
xmake
xmake run app_server
```

Open <http://127.0.0.1:8080> and sign in with the local demo credentials `test_user` / `test_password`. Run the test suite from the project root:

```sh
xmake test
```

## Project layout

| Path | Contents |
| --- | --- |
| `src/server/` | Drogon startup, feature routes, authentication middleware, and JSON responses |
| `src/services/`, `src/db/` | Account logic, upload storage, upstream HTTP calls, and SQLite persistence |
| `src/utils/` | Configuration, logging, random IDs, and worker queue |
| `views/` | Drogon CSP templates compiled by Xmake |
| `public/` | Browser JavaScript, Tailwind source and generated CSS, and runtime uploads |
| `tests/` | Catch2 tests |

## Database example

Define the fields, wrap them in `DaoRecord` for a generated ID and timestamps, and give each table a DAO. This example uses a required foreign key from a task to its project.

```cpp
#include "db/base_dao.hpp"

namespace APP_NAMESPACE {

struct ProjectFields {
    static constexpr std::string_view table_name = "projects";
    std::string name;
};
using ProjectRecord = DaoRecord<ProjectFields>;

class ProjectDao final : public BaseDao<ProjectDao, ProjectRecord> {
public:
    static drogon::Task<ProjectDao> init(DatabaseBackendPtr database) {
        return finishInitialization(ProjectDao(std::move(database)));
    }

private:
    explicit ProjectDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

struct TaskFields {
    static constexpr std::string_view table_name = "tasks";
    std::string title;
    RequiredForeign<ProjectRecord> project_id;
};
using TaskRecord = DaoRecord<TaskFields>;

class TaskDao final : public BaseDao<TaskDao, TaskRecord> {
public:
    static drogon::Task<TaskDao> init(DatabaseBackendPtr database) {
        return finishInitialization(TaskDao(std::move(database)));
    }

private:
    explicit TaskDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

}
```

In a coroutine, open one backend and pass it to both DAOs. Initialize the parent table first. Use `":memory:"` for a disposable example or a file path such as `".local/example.sqlite3"` for persistent storage.

```cpp
namespace APP_NAMESPACE {

drogon::Task<> databaseExample() {
    auto database = co_await openSqliteDatabase(":memory:");
    auto projects = co_await ProjectDao::init(database);
    auto tasks = co_await TaskDao::init(database);

    auto project_id = co_await projects.create({{"Website"}});
    auto task_id = co_await tasks.create({{"Write docs", project_id}});

    auto project_tasks = co_await tasks.find(where<&TaskRecord::project_id>(project_id));
    auto task = co_await tasks.findById(task_id, withForeign<&TaskRecord::project_id>(projects));
    if (task) {
        // task->project_id.record() contains the loaded project.
        task->title = "Review docs";
        co_await tasks.update(*task);
    }

    co_await tasks.deleteById(task_id);
}

}
```

Use `Foreign<ProjectRecord>` for a nullable reference. A plain `findById(task_id)` reads the task and its foreign key. Passing `withForeign` also loads the selected project.

## Configuration

Edit [config.json](config.json) to change the listener or demo credentials. The shipped credentials are for local testing. [`.env.example`](.env.example) lists the optional `APP_*` settings, including `APP_JWT_SECRET` to keep existing sessions valid across server restarts. For listener settings, process environment variables take precedence over `.env` and JSON. The default C++ namespace is `App`. Set a different one with `xmake f --app_namespace=MyApp`.

## Main dependencies

- **Drogon** for HTTP routing and server-rendered views
- **SQLite, libsodium, and jwt-cpp** for account storage, password hashing, and signed sessions
- **CPR, simdjson, and Glaze** for outbound HTTP and JSON handling
- **dotenv-cpp** for loading `.env`
- **Tailwind CSS** for styling and **Catch2** for tests

## Development rules

- Keep `.hpp` files beside their `.cpp` implementations. Use four spaces and put opening braces on the declaration line.
- Keep route handlers small. Delegate persistence and outbound calls, use `co_await` for asynchronous work, `jsonResponse` for typed JSON replies, and `std::println` for console output.
- After editing a view or Tailwind source, run `npm ci` and `npm run build:css` with Node.js installed. Include the regenerated `public/demo.css` in the change.
- Keep `.env`, local databases, and runtime uploads out of commits. See [AGENTS.md](AGENTS.md) for the full repository guidelines.
