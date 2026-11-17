// Mephistorghast — the D2RLoader plugin (the mod's code layer).
// For now it only proves our code runs in the game: it logs a line when loaded
// and adds a console command, "mephisto". Open the console with Ctrl + ` once
// enable_console is turned on in d2rloader/config/d2rloader.toml.

#include <D2RLPlugin/api.h>

static constexpr D2RL::PluginInfo MephistorghastInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "mephistorghast",
	.name        = "Mephistorghast",
	.version     = "0.1.0",
	.author      = "Mephistorghast team",
	.description = "Roguelite rules for the Mephistorghast mod.",
	.flags       = D2RL::PluginFlags::Shared,
};

static auto MephistoCommand(D2R::Game::Client* client, const D2RL::ConsoleCommandContext* command, void* userData) noexcept
	-> D2RL::ConsoleCommandResult {
	(void)client;
	(void)userData;
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	command->plugin->WriteConsoleMessage("Mephistorghast plugin 0.1.0 is running.");
	return D2RL::ConsoleCommandResult::Handled;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &MephistorghastInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	context->LogInfo("Mephistorghast plugin loaded.");
	return context->RegisterConsoleCommand("mephisto", MephistoCommand, "Show that the Mephistorghast plugin is running.");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {}
