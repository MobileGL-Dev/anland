package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.util.ArrayList;
import java.util.List;

import org.junit.Test;

/**
 * The desktop script runs as root with values from a preference and from an exported activity's
 * launch Intent, so what matters is the words a shell makes of the command line.
 */
public class DesktopCommandTest {
    /** Splits a command line into words the way a POSIX shell does for quoting and blanks. */
    private static List<String> shellWords(String line) {
        List<String> words = new ArrayList<>();
        StringBuilder word = new StringBuilder();
        boolean inWord = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (c == '\'') {
                inWord = true;
                int end = line.indexOf('\'', i + 1);
                assertTrue("unterminated quote in " + line, end > i);
                word.append(line, i + 1, end);
                i = end;
            } else if (c == '\\') {
                inWord = true;
                word.append(line.charAt(++i));
            } else if (c == ' ') {
                if (inWord) words.add(word.toString());
                word.setLength(0);
                inWord = false;
            } else {
                assertTrue("unquoted shell metacharacter '" + c + "' in " + line,
                        Character.isLetterOrDigit(c) || "=_-./".indexOf(c) >= 0);
                inWord = true;
                word.append(c);
            }
        }
        if (inWord) words.add(word.toString());
        return words;
    }

    @Test
    public void buildsAssignmentsThenTheScriptAndVerb() {
        String line = DesktopCommand.build("/data/user/0/x/files/mobilegl-desktop.sh", DesktopCommand.Verb.UP,
                "/data/local/tmp/anland-mobilegl/display.sock", "DirectGLES", "arch-kde-mgl",
                "/data/adb/modules/anland-daemon/display_daemon", "/data/local/Droidspaces/bin/droidspaces");
        List<String> words = shellWords(line);
        assertEquals(List.of(
                "SOCK=/data/local/tmp/anland-mobilegl/display.sock",
                "BACKEND=DirectGLES",
                "CONTAINER=arch-kde-mgl",
                "DAEMON=/data/adb/modules/anland-daemon/display_daemon",
                "DS=/data/local/Droidspaces/bin/droidspaces",
                "sh", "/data/user/0/x/files/mobilegl-desktop.sh", "up"), words);
    }

    @Test
    public void hostileSocketPathStaysOneWord() {
        String socket = "/tmp/a'; reboot; echo '$(id)` \\";
        List<String> words = shellWords(DesktopCommand.build("/s.sh", DesktopCommand.Verb.DOWN, socket,
                "DirectVulkan", null, null, null));
        assertEquals("SOCK=" + socket, words.get(0));
        assertEquals("down", words.get(words.size() - 1));
        assertEquals(8, words.size());
    }

    @Test
    public void missingSettingsFallBackToDefaults() {
        List<String> words = shellWords(DesktopCommand.build("/s.sh", DesktopCommand.Verb.STATUS, "/x.sock",
                null, "  ", "", null));
        assertEquals("BACKEND=", words.get(1));
        assertEquals("CONTAINER=" + DesktopCommand.DEFAULT_CONTAINER, words.get(2));
        assertEquals("DAEMON=" + DesktopCommand.DEFAULT_DAEMON, words.get(3));
        assertEquals("DS=" + DesktopCommand.DEFAULT_DROIDSPACES, words.get(4));
    }

    @Test
    public void containerMustBeAName() {
        assertEquals("arch-kde-mgl", DesktopCommand.containerOrDefault(" arch-kde-mgl "));
        assertEquals("my.box_2", DesktopCommand.containerOrDefault("my.box_2"));
        assertEquals(DesktopCommand.DEFAULT_CONTAINER, DesktopCommand.containerOrDefault("../etc"));
        assertEquals(DesktopCommand.DEFAULT_CONTAINER, DesktopCommand.containerOrDefault("a b"));
        assertEquals(DesktopCommand.DEFAULT_CONTAINER, DesktopCommand.containerOrDefault(null));
    }
}
