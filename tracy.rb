class Tracy < Formula
    desc "Real-time, nanosecond resolution frame profiler"
    homepage "https://github.com/wolfpld/tracy"
    url "https://github.com/thienlubos/tracy/archive/refs/tags/v0.10-tt.0-6-gdc450e05.tar.gz"
    sha256 "574ae7bef1863d31ae0e39dba4a968dcd18fd731bbd7fa5e7916eb8df427746a"
    license "BSD-3-Clause"
    revision 1
  
    depends_on "pkg-config" => :build
    depends_on "capstone"
    depends_on "freetype"
    depends_on "glfw"
    depends_on "tbb"
  
    on_linux do
      depends_on "dbus"
      depends_on "libxkbcommon"
    end
  
    fails_with gcc: "5" # C++17
  
    def install
      %w[capture csvexport import-chrome update].each do |f|
        system "make", "-C", "#{f}/build/unix", "release", "LEGACY=1"
        bin.install "#{f}/build/unix/#{f}-release" => "tracy-#{f}"
      end
  
      system "make", "-C", "profiler/build/unix", "release", "LEGACY=1"
      bin.install "profiler/build/unix/Tracy-release" => "tracy"
      system "make", "-C", "library/unix", "release", "LEGACY=1"
      lib.install "library/unix/libtracy-release.so" => "libtracy.so"
  
      %w[client common tracy].each do |f|
        (include/"Tracy/#{f}").install Dir["public/#{f}/*.{h,hpp}"]
      end
    end
  
    test do
      port = free_port
      assert_match "Tracy Profiler #{version}", shell_output("#{bin}/tracy --help")
  
      pid = fork do
        exec "#{bin}/tracy", "-p", port.to_s
      end
      sleep 1
    ensure
      Process.kill("TERM", pid)
      Process.wait(pid)
    end
  end
  