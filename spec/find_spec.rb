require "fileutils"

describe 'find' do
    TMP_DIR = "/tmp/ruby-test"

    # Reference body for the "contents similar" tests (see before block).
    SIMILAR_BODY = "alpha bravo charlie delta echo foxtrot golf hotel india\n" * 4

    MAIN_C = <<~C
        #include <stdio.h>

        // TODO: refactor main() into smaller functions.
        int main(void) {
            printf("Hello World\\n");
            return 0;
        }
    C

    puts "Recompiling program..."
    `gcc -Wall -Wextra main.c -o find 2>&1`

    before do
        # Wipe first: actions like "moveto"/"delete" mutate the tree, so every
        # example needs to start from the same state.
        FileUtils.rm_rf(TMP_DIR)

        # Fixture layout (exercises every condition in main.c's help text):
        #
        #   README.md           0644, top level visible file -> "shallow"
        #   main.c              0644, top level similarity reference for src/main.c
        #   bin/run.sh          0755, "perms exec"
        #   src/main.c          0644, "contents contains TODO"
        #   src/main.h          0644, "name endswith .h"
        #   src/util.c          0644, mtime 3 days ago -> "date olderthan 1D"
        #   src/main.o          0644, "name endswith .o" / "not name endswith .o"
        #   docs/guide.md       0644, "contents contains TODO"
        #   docs/.draft.md      0644, hidden file nested in a visible folder -> "hidden"
        #   docs/nested/notes.md  0644, only found when not "shallow"
        #   data/small.txt      0644, "size lessthan 1kb"
        #   data/big.log        0644, ~200kb, mtime 3 days ago -> "size greaterthan 100kb"
        #   similar/original.txt     reference file for "contents similar"
        #   similar/near_copy.txt    1 byte differs -> passes "similar 98"
        #   similar/different.txt    unrelated bytes -> fails "similar 98"
        #   .env                hidden file, "contents contains SECRET"
        #   .config/settings.ini  hidden folder holding a visible file
        #   backup/             empty, destination for "copyto"/"moveto"
        FileUtils.mkdir_p([
            "#{TMP_DIR}/bin",
            "#{TMP_DIR}/src",
            "#{TMP_DIR}/docs/nested",
            "#{TMP_DIR}/data",
            "#{TMP_DIR}/similar",
            "#{TMP_DIR}/backup",
            "#{TMP_DIR}/.config",
        ])

        File.write("#{TMP_DIR}/src/main.c", MAIN_C)
        File.write("#{TMP_DIR}/README.md", "# Test fixture\n\nNot the real project.\n")
        File.write("#{TMP_DIR}/main.c", MAIN_C)
        File.write("#{TMP_DIR}/src/main.h", "#ifndef MAIN_H\n#define MAIN_H\n\nvoid greet(void);\n\n#endif\n")
        File.write("#{TMP_DIR}/src/util.c", "int add(int a, int b) { return a + b; }\n")
        File.binwrite("#{TMP_DIR}/src/main.o", "\x7fELF\x02\x01\x01" + "\x00" * 64)

        File.write("#{TMP_DIR}/bin/run.sh", "#!/bin/sh\necho running\n")

        File.write("#{TMP_DIR}/docs/guide.md", "# Guide\n\nTODO: document the CLI\n")
        File.write("#{TMP_DIR}/docs/.draft.md", "# Draft\nTODO: finish this\n")
        File.write("#{TMP_DIR}/docs/nested/notes.md", "# Notes\n\nnothing to see\n")

        File.write("#{TMP_DIR}/data/small.txt", "tiny\n")
        File.write("#{TMP_DIR}/data/big.log", "log line\n" * 20_000)

        # Positional byte comparison, so keep the lines the same width: only
        # one byte differs between original and near_copy (209/210 = 99.5%).
        File.write("#{TMP_DIR}/similar/original.txt", SIMILAR_BODY)
        File.write("#{TMP_DIR}/similar/near_copy.txt", SIMILAR_BODY.sub("alpha", "alpHa"))
        File.write("#{TMP_DIR}/similar/different.txt", "z" * SIMILAR_BODY.length)

        File.write("#{TMP_DIR}/.env", "SECRET_TOKEN=hunter2\n")
        File.write("#{TMP_DIR}/.config/settings.ini", "[core]\nverbose = true\n")

        # Your umask is 002, so files would land at 0664 and dirs at 0775.
        # Set the bits explicitly or "perms is 644" / "perms is 755" flake.
        Dir.glob("#{TMP_DIR}/**/*", File::FNM_DOTMATCH).sort.each do |path|
            next if [".", ".."].include?(File.basename(path))

            if File.directory?(path)
                File.chmod(0755, path)
            else
                File.chmod(0644, path)
                File.chmod(0755, path) if path.end_with?("bin/run.sh")
            end
        end

        # Date conditions need a known mtime, not "whatever now is".
        three_days_ago = Time.now - (3 * 24 * 60 * 60)
        File.utime(three_days_ago, three_days_ago, "#{TMP_DIR}/src/util.c")
        File.utime(three_days_ago, three_days_ago, "#{TMP_DIR}/data/big.log")
    end

    BIN = "./find"

    puts "Running #{BIN} with Ruby tests..."

    def run_script(args = "", input: nil, merge_stderr: false)
        output, _status = run_capture(args, input: input, merge_stderr: merge_stderr)
        output
    end

    def run_status(args = "", input: nil, merge_stderr: false)
        _output, status = run_capture(args, input: input, merge_stderr: merge_stderr)
        status
    end

    # Opens the program with "r+" so the child always sees EOF (or `input`) on
    # stdin. Without it, "then delete" blocks forever on its [y/N] prompt.
    # stderr is dropped unless merge_stderr is set, so the suite output stays clean.
    def run_capture(args = "", input: nil, merge_stderr: false)
        command = "#{BIN} #{args}"
        command += merge_stderr ? " 2>&1" : " 2>/dev/null"

        raw_output = nil
        IO.popen(command, "r+") do |pipe|
            pipe.write(input) if input
            pipe.close_write
            raw_output = pipe.read
        end
        [raw_output.to_s.split("\n"), $?&.exitstatus]
    end

    it 'prints documentation when called with no args' do
        expect(run_script()).to include(
            "find.c - Find and operate on files with readable syntax."
        )
    end

    it 'finds files in the current directory' do
        expect(run_script("files")).to include("./main.c", "./README.md", "./find")
    end

    it 'finds folders in a specified directory' do
        expect(run_script("folders in #{TMP_DIR}/docs"))
            .to include("#{TMP_DIR}/docs/nested")
    end

    it 'does not search recursively when shallow is used' do
        expect(run_script("files shallow in #{TMP_DIR}"))
            .to match_array([
                "#{TMP_DIR}/.env",
                "#{TMP_DIR}/README.md",
                "#{TMP_DIR}/main.c"
            ])
    end

    it 'only searches for hidden files when hidden is used' do
        expect(run_script("files hidden in #{TMP_DIR}"))
            .to match_array([
                "#{TMP_DIR}/.env",
                "#{TMP_DIR}/docs/.draft.md"
            ])
    end

    it 'finds visble files in hidden directories' do
        expect(run_script("files visible in #{TMP_DIR}"))
            .to include("#{TMP_DIR}/.config/settings.ini")
    end

    it 'only searches for visible folders when visible is used' do
        expect(run_script("folders visible shallow in #{TMP_DIR}"))
            .to match_array([
                "#{TMP_DIR}/backup",
                "#{TMP_DIR}/bin",
                "#{TMP_DIR}/src",
                "#{TMP_DIR}/similar",
                "#{TMP_DIR}/docs",
                "#{TMP_DIR}/data"
            ])
    end

    it 'finds folders that start with a prefix' do
        expect(run_script("folders in #{TMP_DIR} where name startswith s"))
            .to match_array([
                "#{TMP_DIR}/src",
                "#{TMP_DIR}/similar"
            ])
    end

    it 'finds files that end with a suffix' do
        expect(run_script("files in #{TMP_DIR} where name endswith .c"))
            .to match_array([
                "#{TMP_DIR}/src/main.c",
                "#{TMP_DIR}/main.c",
                "#{TMP_DIR}/src/util.c"
            ])
    end

    it 'finds files that contain a substring' do
        expect(run_script("files in #{TMP_DIR} where name contains main"))
            .to match_array([
                "#{TMP_DIR}/src/main.c",
                "#{TMP_DIR}/src/main.h",
                "#{TMP_DIR}/src/main.o",
                "#{TMP_DIR}/main.c"
            ])
    end

    it 'finds files that are executable' do
        expect(run_script("files in #{TMP_DIR} where perms exec")).to include("#{TMP_DIR}/bin/run.sh")
    end

    it 'finds files with 0644 permissions' do
        expect(run_script("files in #{TMP_DIR}/data where perms is 644"))
            .to match_array([
                "#{TMP_DIR}/data/small.txt",
                "#{TMP_DIR}/data/big.log"
            ])
    end

    it 'finds files where contents contains TODO' do
        expect(run_script("files in #{TMP_DIR} where contents contains TODO"))
            .to match_array([
                "#{TMP_DIR}/main.c",
                "#{TMP_DIR}/src/main.c",
                "#{TMP_DIR}/docs/guide.md",
                "#{TMP_DIR}/docs/.draft.md"
            ])
    end

    it 'finds files with similar contents' do
        expect(run_script("files in #{TMP_DIR} where contents similar 98% to #{TMP_DIR}/main.c"))
            .to match_array([ "#{TMP_DIR}/src/main.c", "#{TMP_DIR}/main.c" ])
    end

    it 'finds files with similar contents using the "contents <pct> similar" syntax' do
        expect(run_script("files in #{TMP_DIR} where contents 0.98 similar to #{TMP_DIR}/main.c"))
            .to match_array([ "#{TMP_DIR}/src/main.c", "#{TMP_DIR}/main.c" ])
    end

    # main.c:747 compares with a 2% slack ("similarity >= threshold - 0.02"), so a
    # 100% threshold still matches a file that differs by a single byte.
    it 'matches a one byte difference at a 100% similarity threshold' do
        expect(run_script("files in #{TMP_DIR}/similar where contents similar 100 to #{TMP_DIR}/similar/original.txt"))
            .to match_array([
                "#{TMP_DIR}/similar/original.txt",
                "#{TMP_DIR}/similar/near_copy.txt"
            ])
    end

    it 'excludes dissimilar files from a similarity search' do
        expect(run_script("files in #{TMP_DIR}/similar where contents similar 50 to #{TMP_DIR}/similar/original.txt"))
            .not_to include("#{TMP_DIR}/similar/different.txt")
    end

    it 'finds files that do not match a name substring' do
        result = run_script("files in #{TMP_DIR} where not name contains main")
        expect(result).to include("#{TMP_DIR}/src/util.c", "#{TMP_DIR}/data/small.txt")
        expect(result).not_to include(a_string_matching(/main/))
    end

    it 'finds files that start with a prefix' do
        expect(run_script("files in #{TMP_DIR} where name startswith ut"))
            .to match_array([ "#{TMP_DIR}/src/util.c" ])
    end

    it 'accepts the separated "ends with" form' do
        expect(run_script("files in #{TMP_DIR} where name ends with .h"))
            .to match_array([ "#{TMP_DIR}/src/main.h" ])
    end

    it 'accepts the separated "starts with" form' do
        expect(run_script("files in #{TMP_DIR} where name starts with READ"))
            .to match_array([ "#{TMP_DIR}/README.md" ])
    end

    it 'combines conditions with and' do
        expect(run_script("files in #{TMP_DIR} where name endswith .c and not name contains util"))
            .to match_array([
                "#{TMP_DIR}/main.c",
                "#{TMP_DIR}/src/main.c"
            ])
    end

    # main.c:719 matches name conditions against the directory entry name, so a
    # parent directory in the path never satisfies a name condition.
    it 'matches name conditions against the file name, not the path' do
        expect(run_script("files in #{TMP_DIR}/src where name contains src")).to be_empty
    end

    it 'combines conditions with or' do
        expect(run_script("files in #{TMP_DIR} where name endswith .o or name endswith .h"))
            .to match_array([
                "#{TMP_DIR}/src/main.o",
                "#{TMP_DIR}/src/main.h"
            ])
    end

    it 'finds items, which includes both files and folders' do
        expect(run_script("items shallow in #{TMP_DIR}")).to match_array([
            "#{TMP_DIR}/backup",
            "#{TMP_DIR}/bin",
            "#{TMP_DIR}/.config",
            "#{TMP_DIR}/data",
            "#{TMP_DIR}/docs",
            "#{TMP_DIR}/similar",
            "#{TMP_DIR}/src",
            "#{TMP_DIR}/.env",
            "#{TMP_DIR}/main.c",
            "#{TMP_DIR}/README.md",
        ])
    end

    it 'searches every path given to "in" and "and"' do
        expect(run_script("files in #{TMP_DIR}/src and #{TMP_DIR}/bin"))
            .to match_array([
                "#{TMP_DIR}/src/main.c",
                "#{TMP_DIR}/src/main.h",
                "#{TMP_DIR}/src/main.o",
                "#{TMP_DIR}/src/util.c",
                "#{TMP_DIR}/bin/run.sh",
            ])
    end

    it 'skips a path already covered by an earlier one' do
        expect(run_script("files in #{TMP_DIR}/src and #{TMP_DIR}/src/main.c"))
            .to match_array([
                "#{TMP_DIR}/src/main.c",
                "#{TMP_DIR}/src/main.h",
                "#{TMP_DIR}/src/main.o",
                "#{TMP_DIR}/src/util.c",
            ])
    end

    it 'skips a covering path given before the path it covers' do
        expect(run_script("files in #{TMP_DIR}/src/main.c and #{TMP_DIR}/src"))
            .to match_array([
                "#{TMP_DIR}/src/main.c",
                "#{TMP_DIR}/src/main.h",
                "#{TMP_DIR}/src/main.o",
                "#{TMP_DIR}/src/util.c",
            ])
    end

    it 'prints nothing and succeeds for a path that does not exist' do
        expect(run_script("files in #{TMP_DIR}/does-not-exist")).to be_empty
        expect(run_status("files in #{TMP_DIR}/does-not-exist")).to eq(0)
    end

    it 'finds files larger than a size with units' do
        expect(run_script("files in #{TMP_DIR} where size greaterthan 100kb"))
            .to match_array([ "#{TMP_DIR}/data/big.log" ])
    end

    it 'finds files smaller than a size' do
        result = run_script("files in #{TMP_DIR} where size lessthan 1kb")
        expect(result).to include("#{TMP_DIR}/data/small.txt")
        expect(result).not_to include("#{TMP_DIR}/data/big.log")
    end

    it 'accepts the operator form of the size condition' do
        expect(run_script(%(files in #{TMP_DIR} where size ">" 100kb)))
            .to match_array([ "#{TMP_DIR}/data/big.log" ])
    end

    it 'accepts the separated "greater than" form of the size condition' do
        expect(run_script("files in #{TMP_DIR} where size greater than 100kb"))
            .to match_array([ "#{TMP_DIR}/data/big.log" ])
    end

    it 'returns nothing when no file is large enough' do
        expect(run_script("files in #{TMP_DIR} where size greaterthan 5mb")).to be_empty
    end

    it 'finds files modified before a relative date' do
        expect(run_script("files in #{TMP_DIR} where date olderthan 1D"))
            .to match_array([
                "#{TMP_DIR}/data/big.log",
                "#{TMP_DIR}/src/util.c"
            ])
    end

    it 'finds files modified after a relative date' do
        result = run_script("files in #{TMP_DIR} where date newerthan 1D")
        expect(result).to include("#{TMP_DIR}/src/main.c")
        expect(result).not_to include("#{TMP_DIR}/data/big.log", "#{TMP_DIR}/src/util.c")
    end

    it 'accepts an explicit MM.DD.YY date' do
        today = Time.now.strftime("%m.%d.%y")
        expect(run_script("files in #{TMP_DIR} where date olderthan #{today}"))
            .to match_array([
                "#{TMP_DIR}/data/big.log",
                "#{TMP_DIR}/src/util.c"
            ])
    end

    it 'accepts the operator form of the date condition' do
        expect(run_script(%(files in #{TMP_DIR} where date "<" 1D)))
            .to match_array([
                "#{TMP_DIR}/data/big.log",
                "#{TMP_DIR}/src/util.c"
            ])
    end

    it 'negates a date condition' do
        expect(run_script("files in #{TMP_DIR} where not date olderthan 1D"))
            .not_to include("#{TMP_DIR}/data/big.log", "#{TMP_DIR}/src/util.c")
    end

    it 'finds files that are not executable' do
        result = run_script("files in #{TMP_DIR} where not perms exec")
        expect(result).to include("#{TMP_DIR}/src/main.c")
        expect(result).not_to include("#{TMP_DIR}/bin/run.sh")
    end

    it 'finds folders with 0755 permissions' do
        expect(run_script("folders in #{TMP_DIR} where perms is 755")).to match_array([
            "#{TMP_DIR}/backup",
            "#{TMP_DIR}/bin",
            "#{TMP_DIR}/.config",
            "#{TMP_DIR}/data",
            "#{TMP_DIR}/docs",
            "#{TMP_DIR}/docs/nested",
            "#{TMP_DIR}/similar",
            "#{TMP_DIR}/src",
        ])
    end

    it 'finds a file by its contents' do
        expect(run_script("files in #{TMP_DIR} where contents contains SECRET"))
            .to match_array([ "#{TMP_DIR}/.env" ])
    end

    it 'returns nothing when no file contains the string' do
        expect(run_script("files in #{TMP_DIR} where contents contains NOT_PRESENT")).to be_empty
    end

    it 'finds folders holding more than a number of files' do
        expect(run_script("folders in #{TMP_DIR} where contents contains \">3\" files"))
            .to match_array([ "#{TMP_DIR}/src" ])
    end

    it 'finds folders holding an exact number of files' do
        expect(run_script("folders in #{TMP_DIR} where contents contains 2 files"))
            .to match_array([ "#{TMP_DIR}/data" ])
    end

    it 'counts only the direct children when the count is shallow' do
        expect(run_script("folders in #{TMP_DIR} where contents contains \">2\" files shallow"))
            .to match_array([
                "#{TMP_DIR}/similar",
                "#{TMP_DIR}/src"
            ])
    end

    it 'counts nested items when the count is not shallow' do
        expect(run_script("folders in #{TMP_DIR} where contents contains \">2\" files"))
            .to match_array([
                "#{TMP_DIR}/docs",
                "#{TMP_DIR}/similar",
                "#{TMP_DIR}/src"
            ])
    end

    it 'counts subfolders' do
        expect(run_script("folders in #{TMP_DIR} where contents contains 1 folders"))
            .to match_array([ "#{TMP_DIR}/docs" ])
    end

    it 'counts items instead of files' do
        expect(run_script("folders in #{TMP_DIR} where contents contains \">2\" items"))
            .to match_array([
                "#{TMP_DIR}/docs",
                "#{TMP_DIR}/similar",
                "#{TMP_DIR}/src"
            ])
    end

    it 'negates a folder count condition' do
        expect(run_script("folders in #{TMP_DIR} where not contents contains \">2\" files"))
            .to match_array([
                "#{TMP_DIR}/backup",
                "#{TMP_DIR}/bin",
                "#{TMP_DIR}/.config",
                "#{TMP_DIR}/data",
                "#{TMP_DIR}/docs/nested",
            ])
    end

    it 'combines a folder count with a name condition using or' do
        expect(run_script("folders in #{TMP_DIR} where name contains sim or contents contains \">3\" files"))
            .to match_array([
                "#{TMP_DIR}/similar",
                "#{TMP_DIR}/src"
            ])
    end

    it 'rejects a count condition on a file search' do
        expect(run_script("files in #{TMP_DIR} where contents contains \">2\" files", merge_stderr: true))
            .to include('find: expected "then", "and", or "or" after condition.')
    end

    it 'rejects an unknown type' do
        expect(run_script("bogus", merge_stderr: true))
            .to include('find: <type> must be "files", "folders", or "items".')
    end

    it 'rejects "in" with no path' do
        expect(run_script("files in", merge_stderr: true))
            .to include('find: expected a path after "in".')
    end

    it 'rejects a condition with no value' do
        expect(run_script("files where name contains", merge_stderr: true))
            .to include('find: incomplete or missing condition after "where".')
    end

    it 'rejects a dangling "and"' do
        expect(run_script("files where name contains a and", merge_stderr: true))
            .to include('find: incomplete or missing condition after "where".')
    end

    it 'rejects "then" with no action' do
        expect(run_script("files then", merge_stderr: true))
            .to include('find: expected an action: moveto, copyto, delete, or command.')
    end

    it 'rejects an unknown action' do
        expect(run_script("files then frobnicate", merge_stderr: true))
            .to include('find: expected an action: moveto, copyto, delete, or command.')
    end

    it 'rejects copyto with no destination' do
        expect(run_script("files where name contains a then copyto", merge_stderr: true))
            .to include("find: incomplete action. moveto/copyto require a destination, command requires arguments.")
    end

    it 'rejects extra arguments after an action' do
        expect(run_script("files where name contains a then copyto #{TMP_DIR}/backup extra", merge_stderr: true))
            .to include('find: Unexpected argument after action.')
    end

    it 'rejects "contents contains" on an item search' do
        expect(run_script("items where contents contains x", merge_stderr: true))
            .to include('find: "contents contains" is not valid with type "items".')
    end

    it 'rejects "contents similar" on a folder search' do
        expect(run_script("folders where contents similar 50 to #{TMP_DIR}/main.c", merge_stderr: true))
            .to include('find: "contents similar" is only valid with type "files".')
    end

    it 'rejects hidden and visible together' do
        expect(run_script("files hidden visible", merge_stderr: true).first)
            .to start_with('find: "hidden" and "visible" cannot be used together')
    end

    it 'rejects a lowercase time unit' do
        expect(run_script("files where date olderthan 5y", merge_stderr: true).first)
            .to start_with("find: Invalid time unit case.")
    end

    it 'exits with a failure status when the arguments are invalid' do
        expect(run_status("bogus")).to eq(1)
    end

    it 'exits with a success status when nothing matches' do
        expect(run_status("files in #{TMP_DIR} where name contains nothing_matches_this")).to eq(0)
    end

    it 'copies files to a destination folder' do
        run_script("files in #{TMP_DIR}/src where name endswith .h then copyto #{TMP_DIR}/backup")
        expect(File.read("#{TMP_DIR}/backup/main.h")).to eq(File.read("#{TMP_DIR}/src/main.h"))
        expect(File.exist?("#{TMP_DIR}/src/main.h")).to be true
    end

    it 'moves files to a destination folder' do
        run_script("files in #{TMP_DIR}/src where name endswith .o then moveto #{TMP_DIR}/backup")
        expect(File.exist?("#{TMP_DIR}/backup/main.o")).to be true
        expect(File.exist?("#{TMP_DIR}/src/main.o")).to be false
    end

    it 'creates the destination folder if it does not exist' do
        run_script("files in #{TMP_DIR}/src where name endswith .h then copyto #{TMP_DIR}/new_folder")
        expect(File.exist?("#{TMP_DIR}/new_folder/main.h")).to be true
    end

    it 'deletes files after confirmation' do
        result = run_script("files in #{TMP_DIR}/data then delete", input: "y\n")
        expect(result.first).to include("Delete 2 files?")
        expect(File.exist?("#{TMP_DIR}/data/small.txt")).to be false
        expect(File.exist?("#{TMP_DIR}/data/big.log")).to be false
    end

    it 'keeps files when the confirmation is declined' do
        result = run_script("files in #{TMP_DIR}/data then delete", input: "n\n")
        # The prompt has no trailing newline, so "Aborted." lands on the same line.
        expect(result.join("\n")).to include("[y/N] Aborted.")
        expect(File.exist?("#{TMP_DIR}/data/small.txt")).to be true
    end

    it 'keeps files when the confirmation gets end of file' do
        result = run_script("files in #{TMP_DIR}/data then delete")
        expect(result.join("\n")).to include("[y/N] Aborted.")
        expect(File.exist?("#{TMP_DIR}/data/small.txt")).to be true
    end

    it 'runs a command once per match' do
        expect(run_script("files in #{TMP_DIR}/data where name endswith .txt then command basename {file}"))
            .to match_array([ "small.txt" ])
    end

    it 'expands {file} in a command template' do
        expect(run_script("files in #{TMP_DIR}/data where name endswith .txt then command wc -c {file}"))
            .to match_array([ "5 #{TMP_DIR}/data/small.txt" ])
    end

    it 'joins a path that ends in a slash with a single separator' do
        expect(run_script("files shallow in #{TMP_DIR}/docs/"))
            .to match_array([
                "#{TMP_DIR}/docs/guide.md",
                "#{TMP_DIR}/docs/.draft.md"
            ])
    end

    it 'joins a path with repeated trailing slashes using a single separator' do
        expect(run_script("files shallow in #{TMP_DIR}/docs///"))
            .to match_array([
                "#{TMP_DIR}/docs/guide.md",
                "#{TMP_DIR}/docs/.draft.md"
            ])
    end

    it 'copies into a destination that ends in a slash' do
        run_script("files in #{TMP_DIR}/src where name endswith .h then copyto #{TMP_DIR}/backup/")
        expect(File.exist?("#{TMP_DIR}/backup/main.h")).to be true
    end

    it 'does not double the separator when searching the root' do
        expect(run_script("folders shallow in /"))
            .to include("/etc", "/tmp")
    end

    it 'rejects an unknown condition keyword' do
        expect(run_script("files in #{TMP_DIR} where bogus value", merge_stderr: true))
            .to include('find: unknown condition. Expected: name, size, date, perms, or contents.')
    end

    it 'rejects an unknown condition keyword after a negation' do
        expect(run_script("files in #{TMP_DIR} where not bogus value", merge_stderr: true))
            .to include('find: unknown condition. Expected: name, size, date, perms, or contents.')
    end

    it 'still finds files when a valid condition follows another' do
        expect(run_script("files in #{TMP_DIR} where name endswith .h and not name contains src"))
            .to match_array([ "#{TMP_DIR}/src/main.h" ])
    end
end
