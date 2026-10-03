package com.anland.consumer;

/**
 * The root command line that runs the packaged desktop script (assets/mobilegl-desktop.sh) for the
 * MobileGL desktop: display daemon, container and its Plasma session.
 *
 * <p>Android-free so it is tested on the JVM. Every value is quoted: the socket path comes from a
 * preference and from the launch Intent of an exported activity, and the result runs as root.
 */
final class DesktopCommand {
    static final String DEFAULT_CONTAINER = "arch-kde-mgl";
    static final String DEFAULT_DAEMON = "/data/adb/modules/anland-daemon/display_daemon";
    static final String DEFAULT_DROIDSPACES = "/data/local/Droidspaces/bin/droidspaces";

    enum Verb {
        UP("up"), DOWN("down"), STATUS("status");

        final String word;

        Verb(String word) {
            this.word = word;
        }
    }

    private DesktopCommand() {
    }

    /** A Droidspaces container name, or the default when the setting is empty or not a name. */
    static String containerOrDefault(String name) {
        if (name == null) return DEFAULT_CONTAINER;
        String trimmed = name.trim();
        return trimmed.matches("[A-Za-z0-9][A-Za-z0-9._-]*") ? trimmed : DEFAULT_CONTAINER;
    }

    static String build(String script, Verb verb, String socket, String backend, String container,
                        String daemon, String droidspaces) {
        return "SOCK=" + SuCommand.shellQuote(socket)
                + " BACKEND=" + SuCommand.shellQuote(backend == null ? "" : backend)
                + " CONTAINER=" + SuCommand.shellQuote(containerOrDefault(container))
                + " DAEMON=" + SuCommand.shellQuote(daemon == null || daemon.isEmpty() ? DEFAULT_DAEMON : daemon)
                + " DS=" + SuCommand.shellQuote(droidspaces == null || droidspaces.isEmpty()
                        ? DEFAULT_DROIDSPACES : droidspaces)
                + " sh " + SuCommand.shellQuote(script) + " " + verb.word;
    }
}
