from conan import ConanFile
from conan.tools.cmake import cmake_layout
from conan.tools.files import copy
import os

class XdaqRhxRecipe(ConanFile):
    name = "xdaq-rhx"
    version = "1.0"

    settings = "os", "compiler", "build_type", "arch"
    
    generators = "CMakeDeps", "CMakeToolchain", "VirtualRunEnv"

    def requirements(self):
        self.requires("fmt/10.2.1")
        self.requires("libxdaq/0.10.2")
        self.requires("nlohmann_json/3.11.3")
        self.requires("spdlog/1.13.0")
        self.requires("crashpad/cci.20220219")

    def build_requirements(self):
        self.tool_requires("cmake/[>=3.25.0 <3.30.0]")
        self.tool_requires("ninja/[>=1.12.0]")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        crashpad_dep = self.dependencies["crashpad"]
        src_bin_dir = os.path.join(crashpad_dep.package_folder, "bin")
        dest_bin_dir = self.build_folder
        
        exe_name = "crashpad_handler.exe" if self.settings.os == "Windows" else "crashpad_handler"
        copy(self, exe_name, src=src_bin_dir, dst=dest_bin_dir)