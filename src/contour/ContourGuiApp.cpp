/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <contour/ContourGuiApp.h>

#include <crispy/logstore.h>

#if defined(CONTOUR_FRONTEND_GUI)
#include <contour/Config.h>
#include <contour/Controller.h>
#include <contour/opengl/TerminalWidget.h>
#endif

#if defined(CONTOUR_FRONTEND_GUI)
#include <QtWidgets/QApplication>
#include <QSurfaceFormat>
#endif

#include <iostream>
#include <vector>

using std::bind;
using std::cerr;
using std::get;
using std::holds_alternative;
using std::prev;
using std::string;
using std::string_view;
using std::vector;

using terminal::Process;

using namespace std::string_literals;

namespace CLI = crispy::cli;

namespace contour {

ContourGuiApp::ContourGuiApp() :
    ContourApp()
{
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
}

int ContourGuiApp::run(int argc, char const* argv[])
{
    argc_ = argc;
    argv_ = argv;

    return ContourApp::run(argc, argv);
}

crispy::cli::Command ContourGuiApp::parameterDefinition() const
{
    auto command = ContourApp::parameterDefinition();

    command.children.insert(
        command.children.begin(),
        CLI::Command{
            "terminal",
            "Spawns a new terminal application.",
            CLI::OptionList{
                CLI::Option{"config", CLI::Value{contour::config::defaultConfigFilePath()}, "Path to configuration file to load at startup.", "FILE"},
                CLI::Option{"profile", CLI::Value{""s}, "Terminal Profile to load (overriding config).", "NAME"},
                CLI::Option{"debug", CLI::Value{""s}, "Enables debug logging, using a comma (,) seperated list of tags.", "TAGS"},
                CLI::Option{"live-config", CLI::Value{false}, "Enables live config reloading."},
                CLI::Option{"dump-state-at-exit", CLI::Value{""s}, "Dumps internal state at exit into the given directory. This is for debugging contour.", "PATH"},
                CLI::Option{"early-exit-threshold", CLI::Value{6u}, "If the spawned process exits earlier than the given threshold seconds, an error message will be printed and the window not closed immediately."},
                CLI::Option{"working-directory", CLI::Value{""s}, "Sets initial working directory (overriding config).", "DIRECTORY"},
                CLI::Option{"class", CLI::Value{""s}, "Sets the class part of the WM_CLASS property for the window (overriding config).", "WM_CLASS"},
                CLI::Option{"platform", CLI::Value{""s}, "Sets the QPA platform.", "PLATFORM[:OPTIONS]"},
                CLI::Option{"session", CLI::Value{""s}, "Sets the sessioni ID used for resuming a prior session.", "SESSION_ID"},
                #if defined(__linux__)
                CLI::Option{"display", CLI::Value{""s}, "Sets the X11 display to connect to.", "DISPLAY_ID"},
                #endif
            },
            CLI::CommandList{},
            CLI::CommandSelect::Implicit,
            CLI::Verbatim{"PROGRAM ARGS...", "Executes given program instead of the configuration profided one."}
        }
    );

    return command;
}

int terminalGUI(int argc, char const* argv[], CLI::FlagStore const& _flags)
{
    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    if (auto const filterString = _flags.get<string>("contour.terminal.debug"); !filterString.empty())
    {
        if (filterString == "all")
        {
            for (auto& category: logstore::get())
                category.get().enable();
        }
        else
        {
            auto const filters = crispy::split(filterString, ',');
            for (auto& category: logstore::get())
            {
                category.get().enable(crispy::any_of(filters, [&](string_view filterPattern) -> bool {
                    if (filterPattern.back() != '*')
                        return category.get().name() == filterPattern;
                    // TODO: '*' excludes hidden categories
                    return std::equal(
                        begin(filterPattern),
                        prev(end(filterPattern)),
                        begin(category.get().name())
                    );
                }));
            }
        }
    }

    auto const configPath = QString::fromStdString(_flags.get<string>("contour.terminal.config"));

    auto config =
        configPath.isEmpty() ? contour::config::loadConfig()
                             : contour::config::loadConfigFromFile(configPath.toStdString());

    string const profileName = [&]() {
        if (auto profile = _flags.get<string>("contour.terminal.profile"); !profile.empty())
            return profile;

        if (!config.defaultProfileName.empty())
            return config.defaultProfileName;

        if (config.profiles.size() == 1)
            return config.profiles.begin()->first;

        return ""s;
    }();

    if (!config.profile(profileName))
    {
        auto const s = accumulate(
            begin(config.profiles),
            end(config.profiles),
            ""s,
            [](string const& acc, auto const& profile) -> string {
                return acc.empty() ? profile.first
                                   : fmt::format("{}, {}", acc, profile.first);
            }
        );
        configLogger(fmt::format("No profile with name '{}' found. Available profiles: {}", profileName, s));
    }

    if (auto const wd = _flags.get<string>("contour.terminal.working-directory"); !wd.empty())
        config.profile(profileName)->shell.workingDirectory = FileSystem::path(wd);

    if (configFailures)
        return EXIT_FAILURE;

    auto const liveConfig = _flags.get<bool>("contour.terminal.live-config");
    auto const dumpStateAtExitStr = _flags.get<string>("contour.terminal.dump-state-at-exit");

    std::optional<FileSystem::path> dumpStateAtExit;
    if (!dumpStateAtExitStr.empty())
        dumpStateAtExit = FileSystem::path(dumpStateAtExitStr);

    std::chrono::seconds earlyExitThreshold(_flags.get<unsigned>("contour.terminal.early-exit-threshold"));

    // Possibly override shell to be executed
    if (!_flags.verbatim.empty())
    {
        auto& shell = config.profile(profileName)->shell;
        shell.program = _flags.verbatim.front();
        shell.arguments.clear();
        for (size_t i = 1; i < _flags.verbatim.size(); ++i)
             shell.arguments.push_back(string(_flags.verbatim.at(i)));
    }

    if (auto const wmClass = _flags.get<string>("contour.terminal.class"); !wmClass.empty())
        config.profile(profileName)->wmClass = wmClass;

    auto appName = QString::fromStdString(config.profile(profileName)->wmClass);
    QCoreApplication::setApplicationName(appName);
    QCoreApplication::setOrganizationName("contour");
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);

    // NB: High DPI scaling should be enabled, but that sadly also applies to QOpenGLWidget
    // which makes the text look pixelated on HighDPI screens. We want to apply HighDPI
    // manually in QOpenGLWidget.
    //QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
    #endif

    vector<string> qtArgsStore;
    vector<char const*> qtArgsPtr;
    qtArgsPtr.push_back(argv[0]);

    auto const addQtArgIfSet = [&](string const& key, char const* arg)
    {
        if (string const& s = _flags.get<string>(key); !s.empty())
        {
            qtArgsPtr.push_back(arg);
            qtArgsStore.push_back(s);
            qtArgsPtr.push_back(qtArgsStore.back().c_str());
        }
    };

    addQtArgIfSet("contour.terminal.session", "-session");
    addQtArgIfSet("contour.terminal.platform", "-platform");

    #if defined(__linux__)
    addQtArgIfSet("contour.terminal.display", "-display");
    #endif

    auto qtArgsCount = static_cast<int>(qtArgsPtr.size());
    QApplication app(qtArgsCount, (char**) qtArgsPtr.data());

    QSurfaceFormat::setDefaultFormat(contour::opengl::TerminalWidget::surfaceFormat());

    contour::Controller controller(qtArgsPtr[0], earlyExitThreshold, config, liveConfig, profileName, dumpStateAtExit);
    controller.start();

    // auto const HTS = "\033H";
    // auto const TBC = "\033[g";
    // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

    auto rv = app.exec();

    controller.exit();
    controller.wait();

    if (auto const ec = controller.exitStatus(); ec.has_value())
    {
        if (holds_alternative<Process::NormalExit>(*ec))
            rv = get<Process::NormalExit>(ec.value()).exitCode;
        else if (holds_alternative<Process::SignalExit>(*ec))
            rv = EXIT_FAILURE;
    }

    // printf("\r%s", TBC);
    return rv;
}

int ContourGuiApp::terminalGuiAction()
{
    return terminalGUI(argc_, argv_, parameters());
}

}
