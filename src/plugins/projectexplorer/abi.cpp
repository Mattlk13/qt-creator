// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "abi.h"
#include "projectexplorerconstants.h"

#include <utils/algorithm.h>
#include <utils/environment.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QDebug>
#include <QtEndian>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QSysInfo>

#ifdef WITH_TESTS
#include <QTest>
#include <QFileInfo>
#endif

/*!
    \class ProjectExplorer::Abi

    \brief The Abi class represents the Application Binary Interface (ABI) of
    a target platform.

    \sa ProjectExplorer::Toolchain
*/

namespace ProjectExplorer {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static std::vector<QByteArray> m_registeredOsFlavors;
static std::map<int, QList<Abi::OSFlavor>> m_osToOsFlavorMap;

static QList<Abi::OSFlavor> moveGenericAndUnknownLast(const QList<Abi::OSFlavor> &in)
{
    QList<Abi::OSFlavor> result = in;
    // Move unknown and generic to the back
    if (result.removeOne(Abi::GenericFlavor))
        result.append(Abi::GenericFlavor);
    if (result.removeOne(Abi::UnknownFlavor))
        result.append(Abi::UnknownFlavor);

    return result;
}

static void insertIntoOsFlavorMap(const std::vector<Abi::OS> &oses, const Abi::OSFlavor flavor)
{
    QTC_ASSERT(oses.size() > 0, return);
    for (const Abi::OS &os : oses) {
        auto it = m_osToOsFlavorMap.find(os);
        if (it == m_osToOsFlavorMap.end()) {
            m_osToOsFlavorMap[os] = {flavor};
            continue;
        }
        QList<Abi::OSFlavor> flavors = it->second;
        if (!flavors.contains(flavor)) {
            flavors.append(flavor);
            m_osToOsFlavorMap[os] = moveGenericAndUnknownLast(flavors);
        }
    }
}

static void registerOsFlavor(const Abi::OSFlavor &flavor, const QByteArray &flavorName,
                             const std::vector<Abi::OS> &oses)
{
    const auto pos = static_cast<size_t>(flavor);
    if (m_registeredOsFlavors.size() <= pos)
        m_registeredOsFlavors.resize(pos + 1);

    m_registeredOsFlavors[pos] = flavorName;

    insertIntoOsFlavorMap(oses, flavor);
}

static void setupPreregisteredOsFlavors() {
    m_registeredOsFlavors.resize(static_cast<size_t>(Abi::UnknownFlavor));

    registerOsFlavor(Abi::FreeBsdFlavor, "freebsd", {Abi::OS::BsdOS});
    registerOsFlavor(Abi::NetBsdFlavor, "netbsd", {Abi::OS::BsdOS});
    registerOsFlavor(Abi::OpenBsdFlavor, "openbsd", {Abi::OS::BsdOS});
    registerOsFlavor(Abi::AndroidLinuxFlavor, "android", {Abi::OS::LinuxOS});
    registerOsFlavor(Abi::SolarisUnixFlavor, "solaris", {Abi::OS::UnixOS});
    registerOsFlavor(Abi::WindowsMsvc2005Flavor, "msvc2005", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2008Flavor, "msvc2008", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2010Flavor, "msvc2010", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2012Flavor, "msvc2012", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2013Flavor, "msvc2013", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2015Flavor, "msvc2015", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2017Flavor, "msvc2017", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2019Flavor, "msvc2019", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMsvc2022Flavor, "msvc2022", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsMSysFlavor, "msys", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::WindowsCEFlavor, "ce", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::VxWorksFlavor, "vxworks", {Abi::OS::VxWorks});
    registerOsFlavor(Abi::RtosFlavor, "rtos", {Abi::OS::WindowsOS});
    registerOsFlavor(Abi::GenericFlavor, "generic", {Abi::OS::LinuxOS,
                                                     Abi::OS::DarwinOS,
                                                     Abi::OS::UnixOS,
                                                     Abi::OS::QnxOS,
                                                     Abi::OS::BareMetalOS});
    registerOsFlavor(Abi::PokyFlavor, "poky", {Abi::OS::LinuxOS});
    registerOsFlavor(Abi::UnknownFlavor, "unknown", {Abi::OS::BsdOS,
                                                     Abi::OS::LinuxOS,
                                                     Abi::OS::DarwinOS,
                                                     Abi::OS::UnixOS,
                                                     Abi::OS::WindowsOS,
                                                     Abi::OS::VxWorks,
                                                     Abi::OS::QnxOS,
                                                     Abi::OS::BareMetalOS,
                                                     Abi::OS::UnknownOS});
}

static std::vector<QByteArray> &registeredOsFlavors() {
    if (m_registeredOsFlavors.size() == 0)
        setupPreregisteredOsFlavors();
    return m_registeredOsFlavors;
}

static int indexOfFlavor(const QByteArray &flavor)
{
    return Utils::indexOf(registeredOsFlavors(), [flavor](const QByteArray &f) { return f == flavor; });
}

static Abi::Architecture architectureFromQt()
{
    const QString arch = QSysInfo::buildCpuArchitecture();
    if (arch.startsWith("arm"))
        return Abi::ArmArchitecture;
    if (arch.startsWith("x86") || arch == "i386")
        return Abi::X86Architecture;
    if (arch == "ia64")
        return Abi::ItaniumArchitecture;
    if (arch.startsWith("mips"))
        return Abi::MipsArchitecture;
    if (arch.startsWith("power"))
        return Abi::PowerPCArchitecture;
    if (arch.startsWith("sh")) // Not in Qt documentation!
        return Abi::ShArchitecture;
    if (arch.startsWith("avr32")) // Not in Qt documentation!
        return Abi::Avr32Architecture;
    if (arch.startsWith("avr")) // Not in Qt documentation!
        return Abi::AvrArchitecture;
    if (arch.startsWith("asmjs"))
        return Abi::AsmJsArchitecture;
    if (arch.startsWith("loongarch"))
        return Abi::LoongArchArchitecture;

    return Abi::UnknownArchitecture;
}

static quint8 getUint8(const QByteArray &data, int pos)
{
    return static_cast<quint8>(data.at(pos));
}

static quint32 getLEUint32(const QByteArray &ba, int pos)
{
    Q_ASSERT(ba.size() >= pos + 3);
    return (static_cast<quint32>(static_cast<quint8>(ba.at(pos + 3))) << 24)
            + (static_cast<quint32>(static_cast<quint8>(ba.at(pos + 2)) << 16))
            + (static_cast<quint32>(static_cast<quint8>(ba.at(pos + 1))) << 8)
            + static_cast<quint8>(ba.at(pos));
}

static quint32 getBEUint32(const QByteArray &ba, int pos)
{
    Q_ASSERT(ba.size() >= pos + 3);
    return (static_cast<quint32>(static_cast<quint8>(ba.at(pos))) << 24)
            + (static_cast<quint32>(static_cast<quint8>(ba.at(pos + 1))) << 16)
            + (static_cast<quint32>(static_cast<quint8>(ba.at(pos + 2))) << 8)
            + static_cast<quint8>(ba.at(pos + 3));
}

static quint32 getLEUint16(const QByteArray &ba, int pos)
{
    Q_ASSERT(ba.size() >= pos + 1);
    return (static_cast<quint16>(static_cast<quint8>(ba.at(pos + 1))) << 8) + static_cast<quint8>(ba.at(pos));
}

static quint32 getBEUint16(const QByteArray &ba, int pos)
{
    Q_ASSERT(ba.size() >= pos + 1);
    return (static_cast<quint16>(static_cast<quint8>(ba.at(pos))) << 8) + static_cast<quint8>(ba.at(pos + 1));
}

static Abi macAbiForCpu(quint32 type) {
    switch (type) {
    case 7: // CPU_TYPE_X86, CPU_TYPE_I386
        return Abi(Abi::X86Architecture, Abi::DarwinOS, Abi::GenericFlavor, Abi::MachOFormat, 32);
    case 0x01000000 +  7: // CPU_TYPE_X86_64
        return Abi(Abi::X86Architecture, Abi::DarwinOS, Abi::GenericFlavor, Abi::MachOFormat, 64);
    case 18: // CPU_TYPE_POWERPC
    case 0x01000000 + 18: // CPU_TYPE_POWERPC64
        return Abi(Abi::PowerPCArchitecture, Abi::DarwinOS, Abi::GenericFlavor, Abi::MachOFormat, 32);
    case 12: // CPU_TYPE_ARM
        return Abi(Abi::ArmArchitecture, Abi::DarwinOS, Abi::GenericFlavor, Abi::MachOFormat, 32);
    case 0x01000000 + 12: // CPU_TYPE_ARM64
        return Abi(Abi::ArmArchitecture, Abi::DarwinOS, Abi::GenericFlavor, Abi::MachOFormat, 64);
    default:
        return {};
    }
}

static Abis parseCoffHeader(const QByteArray &data, int pePos)
{
    Abis result;
    if (data.size() < 20)
        return result;

    Abi::Architecture arch = Abi::UnknownArchitecture;
    Abi::OSFlavor flavor = Abi::UnknownFlavor;
    int width = 0;

    // Get machine field from COFF file header
    quint16 machine = getLEUint16(data, 0);
    switch (machine) {
    case 0x01c0: // ARM LE
    case 0x01c2: // ARM or thumb
    case 0x01c4: // ARMv7 thumb
        arch = Abi::ArmArchitecture;
        width = 32;
        break;
    case 0xaa64: // ARM64
        arch = Abi::ArmArchitecture;
        width = 64;
        break;
    case 0x8664: // x86_64
        arch = Abi::X86Architecture;
        width = 64;
        break;
    case 0x014c: // i386
        arch = Abi::X86Architecture;
        width = 32;
        break;
    case 0x0166: // MIPS, little endian
        arch = Abi::MipsArchitecture;
        width = 32;
        break;
    case 0x0200: // ia64
        arch = Abi::ItaniumArchitecture;
        width = 64;
        break;
    }

    if (pePos == 132 || pePos == 124) {
        // 132 (0x84) => GNU ld.bfd
        // 124 (0x7c) => LLVM lld
        flavor = Abi::WindowsMSysFlavor;
    } else if (data.size() >= 24) {
        // Get Major and Minor Image Version from optional header fields
        quint8 minorLinker = data.at(23);
        switch (data.at(22)) {
        case 2:
        case 3: // not yet reached:-)
            flavor = Abi::WindowsMSysFlavor;
            break;
        case 8:
            flavor = Abi::WindowsMsvc2005Flavor;
            break;
        case 9:
            flavor = Abi::WindowsMsvc2008Flavor;
            break;
        case 10:
            flavor = Abi::WindowsMsvc2010Flavor;
            break;
        case 11:
            flavor = Abi::WindowsMsvc2012Flavor;
            break;
        case 12:
            flavor = Abi::WindowsMsvc2013Flavor;
            break;
        case 14:
            if (minorLinker >= quint8(30))
                flavor = Abi::WindowsMsvc2022Flavor;
            else if (minorLinker >= quint8(20))
                flavor = Abi::WindowsMsvc2019Flavor;
            else if (minorLinker >= quint8(10))
                flavor = Abi::WindowsMsvc2017Flavor;
            else
                flavor = Abi::WindowsMsvc2015Flavor;
            break;
        case 15:
            flavor = Abi::WindowsMsvc2019Flavor;
            break;
        case 16:
            flavor = Abi::WindowsMsvc2022Flavor;
            break;
        default: // Keep unknown flavor
            if (minorLinker != 0)
                flavor = Abi::WindowsMSysFlavor; // MSVC seems to avoid using minor numbers
            else
                qWarning("%s: Unknown MSVC flavour encountered.", Q_FUNC_INFO);
            break;
        }
    }

    if (arch != Abi::UnknownArchitecture && width != 0)
        result.append(Abi(arch, Abi::WindowsOS, flavor, Abi::PEFormat, width));

    return result;
}

static Abis abiOf(const QByteArray &data)
{
    Abis result;
    if (data.size() <= 8)
        return result;

    if (data.size() >= 20
            && getUint8(data, 0) == 0x7f && getUint8(data, 1) == 'E' && getUint8(data, 2) == 'L'
            && getUint8(data, 3) == 'F') {
        // ELF format:
        bool isLE = (getUint8(data, 5) == 1);
        quint16 machine = isLE ? getLEUint16(data, 18) : getBEUint16(data, 18);
        quint8 osAbi = getUint8(data, 7);

        Abi::OS os = Abi::UnixOS;
        Abi::OSFlavor flavor = Abi::GenericFlavor;
        // http://www.sco.com/developers/gabi/latest/ch4.eheader.html#elfid
        switch (osAbi) {
#if defined(Q_OS_NETBSD)
        case 0: // NetBSD: ELFOSABI_NETBSD  2, however, NetBSD uses 0
            os = Abi::BsdOS;
            flavor = Abi::NetBsdFlavor;
            break;
#elif defined(Q_OS_OPENBSD)
        case 0: // OpenBSD: ELFOSABI_OPENBSD 12, however, OpenBSD uses 0
            os = Abi::BsdOS;
            flavor = Abi::OpenBsdFlavor;
            break;
#else
        case 0: // no extra info available: Default to Linux:
#endif
        case 3: // Linux:
        case 97: // ARM, also linux most of the time.
            os = Abi::LinuxOS;
            flavor = Abi::GenericFlavor;
            break;
        case 6: // Solaris:
            os = Abi::UnixOS;
            flavor = Abi::SolarisUnixFlavor;
            break;
        case 9: // FreeBSD:
            os = Abi::BsdOS;
            flavor = Abi::FreeBsdFlavor;
            break;
        }

        switch (machine) {
        case 3: // EM_386
            result.append(Abi(Abi::X86Architecture, os, flavor, Abi::ElfFormat, 32));
            break;
        case 8: { // EM_MIPS
            const int width = getUint8(data, 4) == 2 ? 64 : 32;
            result.append(Abi(Abi::MipsArchitecture, os, flavor, Abi::ElfFormat, width));
            break;
        }
        case 20: // EM_PPC
            result.append(Abi(Abi::PowerPCArchitecture, os, flavor, Abi::ElfFormat, 32));
            break;
        case 21: // EM_PPC64
            result.append(Abi(Abi::PowerPCArchitecture, os, flavor, Abi::ElfFormat, 64));
            break;
        case 40: // EM_ARM
            result.append(Abi(Abi::ArmArchitecture, os, flavor, Abi::ElfFormat, 32));
            break;
        case 183: // EM_AARCH64
            result.append(Abi(Abi::ArmArchitecture, os, flavor, Abi::ElfFormat, 64));
            break;
        case 62: // EM_X86_64
            result.append(Abi(Abi::X86Architecture, os, flavor, Abi::ElfFormat, 64));
            break;
        case 42: // EM_SH
            result.append(Abi(Abi::ShArchitecture, os, flavor, Abi::ElfFormat, 32));
            break;
        case 50: // EM_IA_64
            result.append(Abi(Abi::ItaniumArchitecture, os, flavor, Abi::ElfFormat, 64));
            break;
        case 258: // EM_LOONGARCH
            result.append(Abi(Abi::LoongArchArchitecture, os, flavor, Abi::ElfFormat, 64));
            break;
        default:
            ;
        }
    } else if (((getUint8(data, 0) == 0xce || getUint8(data, 0) == 0xcf)
             && getUint8(data, 1) == 0xfa && getUint8(data, 2) == 0xed && getUint8(data, 3) == 0xfe
            )
            ||
            (getUint8(data, 0) == 0xfe && getUint8(data, 1) == 0xed && getUint8(data, 2) == 0xfa
             && (getUint8(data, 3) == 0xce || getUint8(data, 3) == 0xcf)
            )
           ) {
            // Mach-O format (Mac non-fat binary, 32 and 64bit magic):
            quint32 type = (getUint8(data, 1) ==  0xfa) ? getLEUint32(data, 4) : getBEUint32(data, 4);
            result.append(macAbiForCpu(type));
    } else if ((getUint8(data, 0) == 0xbe && getUint8(data, 1) == 0xba
                && getUint8(data, 2) == 0xfe && getUint8(data, 3) == 0xca)
               ||
               (getUint8(data, 0) == 0xca && getUint8(data, 1) == 0xfe
                && getUint8(data, 2) == 0xba && getUint8(data, 3) == 0xbe)
              ) {
        // Mach-0 format Fat binary header:
        bool isLE = (getUint8(data, 0) == 0xbe);
        quint32 count = isLE ? getLEUint32(data, 4) : getBEUint32(data, 4);
        int pos = 8;
        for (quint32 i = 0; i < count; ++i) {
            if (data.size() <= pos + 4)
                break;

            quint32 type = isLE ? getLEUint32(data, pos) : getBEUint32(data, pos);
            result.append(macAbiForCpu(type));
            pos += 20;
        }
    } else if (// Qt 5.15+ https://webassembly.github.io/spec/core/binary/modules.html#binary-module
               (getUint8(data, 0) == 0 && getUint8(data, 1) == 'a'
                && getUint8(data, 2) == 's' && getUint8(data, 3) == 'm')

               // Qt < 5.15 https://llvm.org/docs/BitCodeFormat.html#llvm-ir-magic-number
               || (getUint8(data, 0) == 'B' && getUint8(data, 1) == 'C'
                   && getUint8(data, 2) == 0xc0 && getUint8(data, 3) == 0xde)) {
        result.append(Abi(Abi::AsmJsArchitecture, Abi::UnknownOS, Abi::UnknownFlavor,
                          Abi::EmscriptenFormat, 32));
    } else if (data.size() >= 64){
        // Windows PE: values are LE (except for a few exceptions which we will not use here).

        // MZ header first (ZM is also allowed, but rarely used)
        const quint8 firstChar = getUint8(data, 0);
        const quint8 secondChar = getUint8(data, 1);
        if ((firstChar != 'M' || secondChar != 'Z') && (firstChar != 'Z' || secondChar != 'M'))
            return result;

        // Get PE/COFF header position from MZ header:
        qint32 pePos = getLEUint32(data, 60);
        if (pePos <= 0 || data.size() < pePos + 4 + 20) // PE magic bytes plus COFF header
            return result;
        if (getUint8(data, pePos) == 'P' && getUint8(data, pePos + 1) == 'E'
            && getUint8(data, pePos + 2) == 0 && getUint8(data, pePos + 3) == 0)
            result = parseCoffHeader(data.mid(pePos + 4), pePos + 4);
    }
    return result;
}

// --------------------------------------------------------------------------
// Abi
// --------------------------------------------------------------------------

Abi::Abi(const Architecture &a, const OS &o,
         const OSFlavor &of, const BinaryFormat &f, unsigned char w, const QString &p) :
    m_architecture(a), m_os(o), m_osFlavor(of), m_binaryFormat(f), m_wordWidth(w), m_param(p)
{
    QTC_ASSERT(osSupportsFlavor(o, of), m_osFlavor = UnknownFlavor);
}

Abi Abi::abiFromTargetTriplet(const QString &triple)
{
    const QString machine = triple.toLower();
    if (machine.isEmpty())
        return {};

    static const QRegularExpression splitRegexp("[ /-]");
    const QStringList parts = machine.split(splitRegexp);

    Architecture arch = UnknownArchitecture;
    OS os = UnknownOS;
    OSFlavor flavor = UnknownFlavor;
    BinaryFormat format = UnknownFormat;
    unsigned char width = 0;

    for (const QString &p : parts) {
        if (p == "unknown" || p == "pc"
                || p == "gnu" || p == "uclibc"
                || p == "86_64" || p == "redhat"
                || p == "w64") {
            continue;
        } else if (p == "i386" || p == "i486" || p == "i586"
                   || p == "i686" || p == "x86") {
            arch = X86Architecture;
            width = 32;
        } else if (p == "xtensa") {
            arch = XtensaArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 32;
        } else if (p.startsWith("arm")) {
            arch = ArmArchitecture;
            width = p.contains("64") ? 64 : 32;
        } else if (p.startsWith("aarch64")) {
            arch = ArmArchitecture;
            width = 64;
        } else if (p == "avr") {
            arch = AvrArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 16;
        } else if (p == "avr32") {
            arch = Avr32Architecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 32;
        } else if (p == "cr16") {
            arch = CR16Architecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            // Note that GCC macro returns 32-bit value for this architecture.
            width = 32;
        } else if (p == "msp430") {
            arch = Msp430Architecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 16;
        } else if (p == "rl78") {
            arch = Rl78Architecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 16;
        } else if (p == "rx") {
            arch = RxArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 32;
        } else if (p == "sh") {
            arch = ShArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 32;
        } else if (p == "v850") {
            arch = V850Architecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 32;
        } else if (p == "m68k") {
            arch = M68KArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 16;
        } else if (p == "m32c") {
            arch = M32CArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 16;
        } else if (p == "m32r") {
            arch = M32RArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = 32;
        } else if (p.startsWith("riscv")) {
            arch = RiscVArchitecture;
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
            width = p.contains("64") ? 64 : 32;
        } else if (p.startsWith("mips")) {
            arch = MipsArchitecture;
            width = p.contains("64") ? 64 : 32;
        } else if (p == "x86_64" || p == "amd64") {
            arch = X86Architecture;
            width = 64;
        } else if (p == "powerpc64") {
            arch = PowerPCArchitecture;
            width = 64;
        } else if (p == "powerpc") {
            arch = PowerPCArchitecture;
            width = 32;
        } else if (p == "linux" || p == "linux6e") {
            os = LinuxOS;
            if (flavor == UnknownFlavor)
                flavor = GenericFlavor;
            format = ElfFormat;
        } else if (p == "android" || p == "androideabi") {
            flavor = AndroidLinuxFlavor;
        } else if (p.startsWith("freebsd")) {
            os = BsdOS;
            if (flavor == UnknownFlavor)
                flavor = FreeBsdFlavor;
            format = ElfFormat;
        } else if (p.startsWith("openbsd")) {
            os = BsdOS;
            if (flavor == UnknownFlavor)
                flavor = OpenBsdFlavor;
            format = ElfFormat;
        } else if (p == "mingw32" || p == "win32"
                   || p == "mingw32msvc" || p == "msys"
                   || p == "cygwin" || p == "windows") {
            if (arch == UnknownArchitecture)
                arch = X86Architecture;
            os = WindowsOS;
            flavor = WindowsMSysFlavor;
            format = PEFormat;
        } else if (p == "apple") {
            os = DarwinOS;
            flavor = GenericFlavor;
            format = MachOFormat;
        } else if (p == "darwin10") {
            width = 64;
        } else if (p == "darwin9") {
            width = 32;
        } else if (p == "gnueabi" || p == "elf") {
            format = ElfFormat;
        } else if (p == "wrs") {
            continue;
        } else if (p == "vxworks") {
            os = VxWorks;
            flavor = VxWorksFlavor;
            format = ElfFormat;
        } else if (p.startsWith("qnx")) {
            os = QnxOS;
            flavor = GenericFlavor;
            format = ElfFormat;
        } else if (p.startsWith("emscripten")) {
            format = EmscriptenFormat;
            width = 32;
        } else if (p.startsWith("asmjs")) {
            arch = AsmJsArchitecture;
        } else if (p == "poky") {
            os = LinuxOS;
            flavor = PokyFlavor;
        } else if (p == "none") {
            os = BareMetalOS;
            flavor = GenericFlavor;
            format = ElfFormat;
        } else if (p == "loongarch64") {
            arch = LoongArchArchitecture;
            width = 64;
        }
    }

    return Abi(arch, os, flavor, format, width);
}

Utils::OsType Abi::abiOsToOsType(const Abi::OS os)
{
    switch (os) {
    case ProjectExplorer::Abi::LinuxOS:
        return Utils::OsType::OsTypeLinux;
    case ProjectExplorer::Abi::DarwinOS:
        return Utils::OsType::OsTypeMac;
    case ProjectExplorer::Abi::BsdOS:
    case ProjectExplorer::Abi::UnixOS:
        return Utils::OsType::OsTypeOtherUnix;
    case ProjectExplorer::Abi::WindowsOS:
        return Utils::OsType::OsTypeWindows;
    case ProjectExplorer::Abi::VxWorks:
    case ProjectExplorer::Abi::QnxOS:
    case ProjectExplorer::Abi::BareMetalOS:
    case ProjectExplorer::Abi::UnknownOS:
        return Utils::OsType::OsTypeOther;
    }
    return Utils::OsType::OsTypeOther;
}

QString Abi::toString() const
{
    const QStringList dn = {toString(m_architecture), toString(m_os), toString(m_osFlavor),
                            toString(m_binaryFormat), toString(m_wordWidth)};
    return dn.join('-');
}

QString Abi::param() const
{
    if (m_param.isEmpty())
        return toString();
    return m_param;
}

QString Abi::toAndroidAbi() const
{
    if (architecture() == Abi::Architecture::ArmArchitecture) {
        if (wordWidth() == 32)
            return Constants::ANDROID_ABI_ARMEABI_V7A;
        if (wordWidth() == 64)
            return Constants::ANDROID_ABI_ARM64_V8A;
    } else if (architecture() == Abi::Architecture::X86Architecture) {
        if (wordWidth() == 32)
            return Constants::ANDROID_ABI_X86;
        if (wordWidth() == 64)
            return Constants::ANDROID_ABI_X86_64;
    }
    return {};
}

bool Abi::operator != (const Abi &other) const
{
    return !operator ==(other);
}

bool Abi::operator == (const Abi &other) const
{
    return m_architecture == other.m_architecture
            && m_os == other.m_os
            && m_osFlavor == other.m_osFlavor
            && m_binaryFormat == other.m_binaryFormat
            && m_wordWidth == other.m_wordWidth;
}

static bool compatibleMSVCFlavors(const Abi::OSFlavor &left, const Abi ::OSFlavor &right)
{
    // MSVC 2022, 2019, 2017 and 2015 are compatible
    return left >= Abi::WindowsMsvc2015Flavor && left <= Abi::WindowsMsvc2022Flavor
           && right >= Abi::WindowsMsvc2015Flavor && right <= Abi::WindowsMsvc2022Flavor;
}

bool Abi::isCompatibleWith(const Abi &other) const
{
    // Generic match: If stuff is identical or the other side is unknown, then this is a match.
    bool isCompat = (architecture() == other.architecture() || other.architecture() == UnknownArchitecture)
                     && (os() == other.os() || other.os() == UnknownOS)
                     && (osFlavor() == other.osFlavor() || other.osFlavor() == UnknownFlavor)
                     && (binaryFormat() == other.binaryFormat() || other.binaryFormat() == UnknownFormat)
                     && ((wordWidth() == other.wordWidth() && wordWidth() != 0) || other.wordWidth() == 0);

    // *-linux-generic-* is compatible with *-linux-* (both ways): This is for the benefit of
    // people building Qt themselves using e.g. a meego toolchain.
    //
    // We leave it to the specific targets to filter out the tool chains that do not
    // work for them.
    if (!isCompat && (architecture() == other.architecture() || other.architecture() == UnknownArchitecture)
                  && ((os() == other.os()) && (os() == LinuxOS))
                  && (osFlavor() == GenericFlavor || other.osFlavor() == GenericFlavor)
                  && (binaryFormat() == other.binaryFormat() || other.binaryFormat() == UnknownFormat)
                  && ((wordWidth() == other.wordWidth() && wordWidth() != 0) || other.wordWidth() == 0)) {
        isCompat = true;
    }

    // Make Android matching more strict than the generic Linux matches so far:
    if (isCompat && (osFlavor() == AndroidLinuxFlavor || other.osFlavor() == AndroidLinuxFlavor))
        isCompat = (architecture() == other.architecture()) &&  (osFlavor() == other.osFlavor());

    if (!isCompat && wordWidth() == other.wordWidth()
            && compatibleMSVCFlavors(osFlavor(), other.osFlavor())) {
        isCompat = true;
    }

    return isCompat;
}

bool Abi::isValid() const
{
    if (m_architecture == UnknownArchitecture || m_binaryFormat == UnknownFormat
        || m_wordWidth == 0) {
        return false;
    }

    return m_architecture == AsmJsArchitecture
           || (m_os != UnknownOS && m_osFlavor != UnknownFlavor);
}

bool Abi::isNull() const
{
    return m_architecture == UnknownArchitecture
            && m_os == UnknownOS
            && m_osFlavor == UnknownFlavor
            && m_binaryFormat == UnknownFormat
            && m_wordWidth == 0;
}

QString Abi::toString(const Architecture &a)
{
    switch (a) {
    case ArmArchitecture:
        return QLatin1String("arm");
    case AvrArchitecture:
        return QLatin1String("avr");
    case Avr32Architecture:
        return QLatin1String("avr32");
    case XtensaArchitecture:
        return QLatin1String("xtensa");
    case X86Architecture:
        return QLatin1String("x86");
    case Mcs51Architecture:
        return QLatin1String("mcs51");
    case Mcs251Architecture:
        return QLatin1String("mcs251");
    case MipsArchitecture:
        return QLatin1String("mips");
    case PowerPCArchitecture:
        return QLatin1String("ppc");
    case ItaniumArchitecture:
        return QLatin1String("itanium");
    case ShArchitecture:
        return QLatin1String("sh");
    case AsmJsArchitecture:
        return QLatin1String("asmjs");
    case Stm8Architecture:
        return QLatin1String("stm8");
    case Msp430Architecture:
        return QLatin1String("msp430");
    case Rl78Architecture:
        return QLatin1String("rl78");
    case C166Architecture:
        return QLatin1String("c166");
    case V850Architecture:
        return QLatin1String("v850");
    case Rh850Architecture:
        return QLatin1String("rh850");
    case RxArchitecture:
        return QLatin1String("rx");
    case K78Architecture:
        return QLatin1String("78k");
    case M68KArchitecture:
        return QLatin1String("m68k");
    case M32CArchitecture:
        return QLatin1String("m32c");
    case M16CArchitecture:
        return QLatin1String("m16c");
    case M32RArchitecture:
        return QLatin1String("m32r");
    case R32CArchitecture:
        return QLatin1String("r32c");
    case CR16Architecture:
        return QLatin1String("cr16");
    case RiscVArchitecture:
        return QLatin1String("riscv");
    case LoongArchArchitecture:
        return QLatin1String("loongarch");
    case UnknownArchitecture:
        Q_FALLTHROUGH();
    default:
        return QLatin1String("unknown");
    }
}

QString Abi::toString(const OS &o)
{
    switch (o) {
    case LinuxOS:
        return QLatin1String("linux");
    case BsdOS:
        return QLatin1String("bsd");
    case DarwinOS:
        return QLatin1String("darwin");
    case UnixOS:
        return QLatin1String("unix");
    case WindowsOS:
        return QLatin1String("windows");
    case VxWorks:
        return QLatin1String("vxworks");
    case QnxOS:
        return QLatin1String("qnx");
    case BareMetalOS:
        return QLatin1String("baremetal");
    case UnknownOS:
        Q_FALLTHROUGH();
    default:
        return QLatin1String("unknown");
    };
}

QString Abi::toString(const OSFlavor &of)
{
    const auto index = static_cast<size_t>(of);
    const std::vector<QByteArray> &flavors = registeredOsFlavors();
    QTC_ASSERT(index < flavors.size(),
               return QString::fromUtf8(flavors.at(int(UnknownFlavor))));
    return QString::fromUtf8(flavors.at(index));
}

QString Abi::toString(const BinaryFormat &bf)
{
    switch (bf) {
    case ElfFormat:
        return QLatin1String("elf");
    case PEFormat:
        return QLatin1String("pe");
    case MachOFormat:
        return QLatin1String("mach_o");
    case RuntimeQmlFormat:
        return QLatin1String("qml_rt");
    case UbrofFormat:
        return QLatin1String("ubrof");
    case OmfFormat:
        return QLatin1String("omf");
    case EmscriptenFormat:
        return QLatin1String("emscripten");
    case UnknownFormat:
        Q_FALLTHROUGH();
    default:
        return QLatin1String("unknown");
    }
}

QString Abi::toString(int w)
{
    if (w == 0)
        return QLatin1String("unknown");
    return QString::fromLatin1("%1bit").arg(w);
}

Abi Abi::fromString(const QString &abiString)
{
    Abi::Architecture architecture = UnknownArchitecture;
    const QStringList abiParts = abiString.split('-');
    if (!abiParts.isEmpty()) {
        architecture = architectureFromString(abiParts.at(0));
        if (abiParts.at(0) != toString(architecture))
            return {};
    }

    Abi::OS os = UnknownOS;
    if (abiParts.count() >= 2) {
        os = osFromString(abiParts.at(1));
        if (abiParts.at(1) != toString(os))
            return Abi(architecture, UnknownOS, UnknownFlavor, UnknownFormat, 0);
    }

    Abi::OSFlavor flavor = UnknownFlavor;
    if (abiParts.count() >= 3) {
        flavor = osFlavorFromString(abiParts.at(2), os);
        if (abiParts.at(2) != toString(flavor))
            return Abi(architecture, os, UnknownFlavor, UnknownFormat, 0);
    }

    Abi::BinaryFormat format = UnknownFormat;
    if (abiParts.count() >= 4) {
        format = binaryFormatFromString(abiParts.at(3));
        if (abiParts.at(3) != toString(format))
            return Abi(architecture, os, flavor, UnknownFormat, 0);
    }

    unsigned char wordWidth = 0;
    if (abiParts.count() >= 5) {
        wordWidth = wordWidthFromString(abiParts.at(4));
        if (abiParts.at(4) != toString(wordWidth))
            return Abi(architecture, os, flavor, format, 0);
    }

    Abi abi(architecture, os, flavor, format, wordWidth);
    if (abi.os() == LinuxOS && abi.osFlavor() == AndroidLinuxFlavor)
        abi.m_param = abi.toAndroidAbi();

    return abi;
}

Abi::Architecture Abi::architectureFromString(const QString &a)
{
    if (a == "unknown")
        return UnknownArchitecture;
    if (a == "arm")
        return ArmArchitecture;
    if (a == "aarch64")
        return ArmArchitecture;
    if (a == "avr")
        return AvrArchitecture;
    if (a == "avr32")
        return Avr32Architecture;
    if (a == "x86")
        return X86Architecture;
    if (a == "mcs51")
        return Mcs51Architecture;
    if (a == "mcs251")
        return Mcs251Architecture;
    if (a == "mips")
        return MipsArchitecture;
    if (a == "ppc")
        return PowerPCArchitecture;
    if (a == "itanium")
        return ItaniumArchitecture;
    if (a == "sh")
        return ShArchitecture;
    if (a == "stm8")
        return Stm8Architecture;
    if (a == "msp430")
        return Msp430Architecture;
    if (a == "rl78")
        return Rl78Architecture;
    if (a == "c166")
        return C166Architecture;
    if (a == "v850")
        return V850Architecture;
    if (a == "rh850")
        return Rh850Architecture;
    if (a == "rx")
        return RxArchitecture;
    if (a == "78k")
        return K78Architecture;
    if (a == "m68k")
        return M68KArchitecture;
    if (a == "m32c")
        return M32CArchitecture;
    if (a == "m16c")
        return M16CArchitecture;
    if (a == "m32r")
        return M32RArchitecture;
    if (a == "r32c")
        return R32CArchitecture;
    if (a == "cr16")
        return CR16Architecture;
    if (a == "riscv")
        return RiscVArchitecture;
    else if (a == "xtensa")
        return XtensaArchitecture;
    if (a == "asmjs")
        return AsmJsArchitecture;
    if (a == "loongarch")
        return LoongArchArchitecture;

    return UnknownArchitecture;
}

Abi::OS Abi::osFromString(const QString &o)
{
    if (o == "unknown")
        return UnknownOS;
    if (o == "linux")
        return LinuxOS;
    if (o == "bsd")
        return BsdOS;
    if (o == "darwin" || o == "macos")
        return DarwinOS;
    if (o == "unix")
        return UnixOS;
    if (o == "windows")
        return WindowsOS;
    if (o == "vxworks")
        return VxWorks;
    if (o == "qnx")
        return QnxOS;
    if (o == "baremetal")
        return BareMetalOS;
    return UnknownOS;
}

Abi::OSFlavor Abi::osFlavorFromString(const QString &of, const OS os)
{
    const int index = indexOfFlavor(of.toUtf8());
    const auto flavor = OSFlavor(index);
    if (index >= 0 && osSupportsFlavor(os, flavor))
        return flavor;
    return UnknownFlavor;
}

Abi::BinaryFormat Abi::binaryFormatFromString(const QString &bf)
{
    if (bf == "unknown")
        return UnknownFormat;
    if (bf == "elf")
        return ElfFormat;
    if (bf == "pe")
        return PEFormat;
    if (bf == "mach_o")
        return MachOFormat;
    if (bf == "ubrof")
        return UbrofFormat;
    if (bf == "omf")
        return OmfFormat;
    if (bf == "qml_rt")
        return RuntimeQmlFormat;
    if (bf == "emscripten")
        return EmscriptenFormat;
    return UnknownFormat;
}

unsigned char Abi::wordWidthFromString(const QString &w)
{
    if (!w.endsWith("bit"))
        return 0;

    bool ok = false;
    const QString number = w.left(w.size() - 3);
    const int bitCount = number.toInt(&ok);
    if (!ok)
        return 0;
    if (bitCount != 8 && bitCount != 16 && bitCount != 32 && bitCount != 64)
        return 0;
    return static_cast<unsigned char>(bitCount);
}

Abi::OSFlavor Abi::registerOsFlavor(const std::vector<OS> &oses, const QString &flavorName)
{
    QTC_ASSERT(oses.size() > 0, return UnknownFlavor);
    const QByteArray flavorBytes = flavorName.toUtf8();

    int index = indexOfFlavor(flavorBytes);
    if (index < 0)
        index = int(registeredOsFlavors().size());

    auto toRegister = OSFlavor(index);
    ProjectExplorer::registerOsFlavor(toRegister, flavorBytes, oses);
    return toRegister;
}

QList<Abi::OSFlavor> Abi::flavorsForOs(const Abi::OS &o)
{
    registeredOsFlavors(); // Make sure m_osToOsFlavorMap is populated!
    auto it = m_osToOsFlavorMap.find(o);
    if (it == m_osToOsFlavorMap.end())
        return {};

    return it->second;
}

QList<Abi::OSFlavor> Abi::allOsFlavors()
{
    QList<OSFlavor> result;
    for (size_t i = 0; i < registeredOsFlavors().size(); ++i)
        result << OSFlavor(i);
    return moveGenericAndUnknownLast(result);
}

bool Abi::osSupportsFlavor(const Abi::OS &os, const Abi::OSFlavor &flavor)
{
    return flavorsForOs(os).contains(flavor);
}

Abi::OSFlavor Abi::flavorForMsvcVersion(int version)
{
    if (version >= 1930)
        return WindowsMsvc2022Flavor;
    if (version >= 1920)
        return WindowsMsvc2019Flavor;
    if (version >= 1910)
        return WindowsMsvc2017Flavor;
    switch (version) {
    case 1900:
        return WindowsMsvc2015Flavor;
    case 1800:
        return WindowsMsvc2013Flavor;
    case 1700:
        return WindowsMsvc2012Flavor;
    case 1600:
        return WindowsMsvc2010Flavor;
    case 1500:
        return WindowsMsvc2008Flavor;
    case 1400:
        return WindowsMsvc2005Flavor;
    default:
        return WindowsMSysFlavor;
    }
}

Abi Abi::hostAbi()
{
    Architecture arch = architectureFromQt();
    OS os = UnknownOS;
    OSFlavor subos = UnknownFlavor;
    BinaryFormat format = UnknownFormat;

#if defined (Q_OS_WIN)
    os = WindowsOS;
#ifdef _MSC_VER
    subos = flavorForMsvcVersion(_MSC_VER);
#elif defined (Q_CC_MINGW)
    subos = WindowsMSysFlavor;
#endif
    format = PEFormat;
#elif defined (Q_OS_LINUX)
    os = LinuxOS;
    subos = GenericFlavor;
    format = ElfFormat;
#elif defined (Q_OS_DARWIN)
    os = DarwinOS;
    subos = GenericFlavor;
    format = MachOFormat;
#elif defined (Q_OS_BSD4)
    os = BsdOS;
# if defined (Q_OS_FREEBSD)
    subos = FreeBsdFlavor;
# elif defined (Q_OS_NETBSD)
    subos = NetBsdFlavor;
# elif defined (Q_OS_OPENBSD)
    subos = OpenBsdFlavor;
# endif
    format = ElfFormat;
#endif

    const Abi result(arch, os, subos, format, QSysInfo::WordSize);
    if (!result.isValid())
        qWarning("Unable to completely determine the host ABI (%s).",
                 qPrintable(result.toString()));
    return result;
}

//! Extract available ABIs from a binary using heuristics.
Abis Abi::abisOfBinary(const Utils::FilePath &path)
{
    Abis tmp;
    if (path.isEmpty())
        return tmp;

    QByteArray data = path.fileContents(1024).value_or(QByteArray());
    if (data.size() >= 67
            && getUint8(data, 0) == '!' && getUint8(data, 1) == '<' && getUint8(data, 2) == 'a'
            && getUint8(data, 3) == 'r' && getUint8(data, 4) == 'c' && getUint8(data, 5) == 'h'
            && getUint8(data, 6) == '>' && getUint8(data, 7) == 0x0a) {
        // We got an ar file: possibly a static lib for ELF, PE or Mach-O

        data = data.mid(8); // Cut of ar file magic
        quint64 offset = 8;

        while (!data.isEmpty()) {
            if ((getUint8(data, 58) != 0x60 || getUint8(data, 59) != 0x0a)) {
                qWarning() << path.toUrlishString() << ": Thought it was an ar-file, but it is not!";
                break;
            }

            const QString fileName = QString::fromLocal8Bit(data.mid(0, 16));
            quint64 fileNameOffset = 0;
            if (fileName.startsWith("#1/"))
                fileNameOffset = fileName.mid(3).toInt();
            const QString fileLength = QString::fromLatin1(data.mid(48, 10));

            int toSkip = 60 + fileNameOffset;
            offset += fileLength.toInt() + 60 /* header */;

            tmp.append(abiOf(data.mid(toSkip)));
            if (tmp.isEmpty() && fileName == "/0              ") {
                tmp = parseCoffHeader(data.mid(toSkip, 20), toSkip); // This might be windws...
                if (tmp.isEmpty()) {
                    // Qt 6.2 static builds have the coff headers for both MSVC and MinGW at offset 66
                    toSkip = 66 + fileNameOffset;
                    tmp = parseCoffHeader(data.mid(toSkip, 20), toSkip);
                }
            }

            if (!tmp.isEmpty() && tmp.at(0).binaryFormat() != MachOFormat)
                break;

            offset += (offset % 2); // ar is 2 byte aligned
            data = path.fileContents(1024, offset).value_or(QByteArray());
        }
    } else {
        tmp = abiOf(data);
    }

    // Remove duplicates:
    Abis result;
    for (const Abi &a : std::as_const(tmp)) {
        if (!result.contains(a))
            result.append(a);
    }

    return result;
}

#ifdef WITH_TESTS
namespace Internal {

static bool isGenericFlavor(Abi::OSFlavor f) { return f == Abi::GenericFlavor; }

class AbiTest : public QObject
{
    Q_OBJECT

private slots:
    void testRoundTrips()
    {
        for (int i = 0; i <= Abi::UnknownArchitecture; ++i) {
            const QString string = Abi::toString(static_cast<Abi::Architecture>(i));
            const Abi::Architecture arch = Abi::architectureFromString(string);
            QCOMPARE(static_cast<Abi::Architecture>(i), arch);
        }
        for (int i = 0; i <= Abi::UnknownOS; ++i) {
            const QString string = Abi::toString(static_cast<Abi::OS>(i));
            const Abi::OS os = Abi::osFromString(string);
            QCOMPARE(static_cast<Abi::OS>(i), os);
        }
        for (const Abi::OSFlavor flavorIt : Abi::allOsFlavors()) {
            const QString string = Abi::toString(flavorIt);
            for (int os = 0; os <= Abi::UnknownOS; ++os) {
                const auto osEnum = static_cast<Abi::OS>(os);
                const Abi::OSFlavor flavor = Abi::osFlavorFromString(string, osEnum);
                if (isGenericFlavor(flavorIt) && flavor != Abi::UnknownFlavor)
                    QVERIFY(isGenericFlavor(flavor));
                else if (flavor == Abi::UnknownFlavor && flavorIt != Abi::UnknownFlavor)
                    QVERIFY(!Abi::flavorsForOs(osEnum).contains(flavorIt));
                else
                    QCOMPARE(flavorIt, flavor);
            }
        }
        for (int i = 0; i <= Abi::UnknownFormat; ++i) {
            QString string = Abi::toString(static_cast<Abi::BinaryFormat>(i));
            Abi::BinaryFormat format = Abi::binaryFormatFromString(string);
            QCOMPARE(static_cast<Abi::BinaryFormat>(i), format);
        }
        for (unsigned char i : {0, 8, 16, 32, 64}) {
            QString string = Abi::toString(i);
            unsigned char wordwidth = Abi::wordWidthFromString(string);
            QCOMPARE(i, wordwidth);
        }
    }

    void testAbiOfBinary_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QStringList>("abis");

        QTest::newRow("no file")
            << QString()
            << (QStringList());
        QTest::newRow("non existing file")
            << QString::fromLatin1("/does/not/exist")
            << (QStringList());

        // Clone test data from: https://git.qt.io/chstenge/creator-test-data
        // Set up prefix for test data now that we can be sure to have some tests to run:
        QString prefix = Utils::qtcEnvironmentVariable("QTC_TEST_EXTRADATALOCATION");
        if (prefix.isEmpty())
            return;
        prefix += "/projectexplorer/abi";

        QFileInfo fi(prefix);
        if (!fi.exists() || !fi.isDir())
            return;
        prefix = fi.absoluteFilePath();

        QTest::newRow("text file")
            << QString::fromLatin1("%1/broken/text.txt").arg(prefix)
            << (QStringList());

        QTest::newRow("static QtCore: win msvc2008")
            << QString::fromLatin1("%1/static/win-msvc2008-release.lib").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-unknown-pe-32bit"));
        QTest::newRow("static QtCore: win msvc2008 II")
            << QString::fromLatin1("%1/static/win-msvc2008-release2.lib").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-unknown-pe-64bit"));
        QTest::newRow("static QtCore: win msvc2008 (debug)")
            << QString::fromLatin1("%1/static/win-msvc2008-debug.lib").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-unknown-pe-32bit"));
        QTest::newRow("static QtCore: win mingw")
            << QString::fromLatin1("%1/static/win-mingw.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-unknown-pe-32bit"));
        QTest::newRow("static QtCore: mac (debug)")
            << QString::fromLatin1("%1/static/mac-32bit-debug.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-darwin-generic-mach_o-32bit"));
        QTest::newRow("static QtCore: linux 32bit")
            << QString::fromLatin1("%1/static/linux-32bit-release.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-linux-generic-elf-32bit"));
        QTest::newRow("static QtCore: linux 64bit")
            << QString::fromLatin1("%1/static/linux-64bit-release.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-linux-generic-elf-64bit"));
        QTest::newRow("static QtCore: asmjs emscripten 32bit (Qt < 5.15)")
            << QString::fromLatin1("%1/static/asmjs-emscripten.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("asmjs-unknown-unknown-emscripten-32bit"));
        QTest::newRow("static QtCore: asmjs emscripten 32bit (Qt >= 5.15)")
            << QString::fromLatin1("%1/static/asmjs-emscripten-5.15.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("asmjs-unknown-unknown-emscripten-32bit"));

        QTest::newRow("static stdc++: mac fat")
            << QString::fromLatin1("%1/static/mac-fat.a").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-darwin-generic-mach_o-32bit")
                              << QString::fromLatin1("ppc-darwin-generic-mach_o-32bit")
                              << QString::fromLatin1("x86-darwin-generic-mach_o-64bit"));

        QTest::newRow("executable: win msvc2013 64bit")
            << QString::fromLatin1("%1/executables/x86-windows-mvsc2013-pe-64bit.exe").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2013-pe-64bit"));
        QTest::newRow("executable: win msvc2013 32bit")
            << QString::fromLatin1("%1/executables/x86-windows-mvsc2013-pe-32bit.exe").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2013-pe-32bit"));
        QTest::newRow("dynamic: win msvc2013 64bit")
            << QString::fromLatin1("%1/dynamic/x86-windows-mvsc2013-pe-64bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2013-pe-64bit"));
        QTest::newRow("dynamic: win msvc2013 32bit")
            << QString::fromLatin1("%1/dynamic/x86-windows-mvsc2013-pe-32bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2013-pe-32bit"));
        QTest::newRow("dynamic QtCore: win msvc2010 64bit")
            << QString::fromLatin1("%1/dynamic/win-msvc2010-64bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2010-pe-64bit"));
        QTest::newRow("dynamic QtCore: win msvc2008 32bit")
            << QString::fromLatin1("%1/dynamic/win-msvc2008-32bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2008-pe-32bit"));
        QTest::newRow("dynamic QtCore: win msvc2005 32bit")
            << QString::fromLatin1("%1/dynamic/win-msvc2005-32bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msvc2005-pe-32bit"));
        QTest::newRow("dynamic QtCore: win msys 32bit")
            << QString::fromLatin1("%1/dynamic/win-mingw-32bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msys-pe-32bit"));
        QTest::newRow("dynamic QtCore: win mingw 64bit")
            << QString::fromLatin1("%1/dynamic/win-mingw-64bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msys-pe-64bit"));
        QTest::newRow("dynamic QtCore: wince mips msvc2005 32bit")
            << QString::fromLatin1("%1/dynamic/wince-32bit.dll").arg(prefix)
            << (QStringList() << QString::fromLatin1("mips-windows-msvc2005-pe-32bit"));
        QTest::newRow("dynamic QtCore: wince arm msvc2008 32bit")
            << QString::fromLatin1("%1/dynamic/arm-win-ce-pe-32bit").arg(prefix)
            << (QStringList() << QString::fromLatin1("arm-windows-msvc2008-pe-32bit"));

        QTest::newRow("dynamic stdc++: mac fat")
            << QString::fromLatin1("%1/dynamic/mac-fat.dylib").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-darwin-generic-mach_o-32bit")
                              << QString::fromLatin1("ppc-darwin-generic-mach_o-32bit")
                              << QString::fromLatin1("x86-darwin-generic-mach_o-64bit"));
        QTest::newRow("dynamic QtCore: arm linux 32bit")
            << QString::fromLatin1("%1/dynamic/arm-linux.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("arm-linux-generic-elf-32bit"));
        QTest::newRow("dynamic QtCore: arm linux 32bit, using ARM as OSABI")
            << QString::fromLatin1("%1/dynamic/arm-linux2.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("arm-linux-generic-elf-32bit"));
        QTest::newRow("dynamic QtCore: arm linux 32bit (angstrom)")
            << QString::fromLatin1("%1/dynamic/arm-angstrom-linux.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("arm-linux-generic-elf-32bit"));
        QTest::newRow("dynamic QtCore: sh4 linux 32bit")
            << QString::fromLatin1("%1/dynamic/sh4-linux.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("sh-linux-generic-elf-32bit"));
        QTest::newRow("dynamic QtCore: mips linux 32bit")
            << QString::fromLatin1("%1/dynamic/mips-linux.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("mips-linux-generic-elf-32bit"));
        QTest::newRow("dynamic QtCore: projectexplorer/abi/static/win-msvc2010-32bit.libppc be linux 32bit")
            << QString::fromLatin1("%1/dynamic/ppcbe-linux-32bit.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("ppc-linux-generic-elf-32bit"));
        QTest::newRow("dynamic QtCore: x86 freebsd 64bit")
            << QString::fromLatin1("%1/dynamic/freebsd-elf-64bit.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-bsd-freebsd-elf-64bit"));
        QTest::newRow("dynamic QtCore: x86 freebsd 64bit")
            << QString::fromLatin1("%1/dynamic/freebsd-elf-64bit.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-bsd-freebsd-elf-64bit"));
        QTest::newRow("dynamic QtCore: x86 freebsd 32bit")
            << QString::fromLatin1("%1/dynamic/freebsd-elf-32bit.so").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-bsd-freebsd-elf-32bit"));

        QTest::newRow("executable: x86 win 32bit cygwin executable")
            << QString::fromLatin1("%1/executable/cygwin-32bit.exe").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msys-pe-32bit"));
        QTest::newRow("executable: x86 win 32bit mingw executable")
            << QString::fromLatin1("%1/executable/mingw-32bit.exe").arg(prefix)
            << (QStringList() << QString::fromLatin1("x86-windows-msys-pe-32bit"));
    }

    void testAbiOfBinary()
    {
        QFETCH(QString, file);
        QFETCH(QStringList, abis);

        const Utils::FilePath fp = Utils::FilePath::fromString(file);
        const QString dataTag = QString::fromLocal8Bit(QTest::currentDataTag());
        if (!fp.exists() && dataTag != "no file" && dataTag != "non existing file")
            QSKIP("binary file not present");

        const Abis result = Abi::abisOfBinary(fp);
        QCOMPARE(result.count(), abis.count());
        for (int i = 0; i < abis.count(); ++i)
            QCOMPARE(result.at(i).toString(), abis.at(i));
    }

    void testAbiFromTargetTriplet_data()
    {
        QTest::addColumn<int>("architecture");
        QTest::addColumn<int>("os");
        QTest::addColumn<int>("osFlavor");
        QTest::addColumn<int>("binaryFormat");
        QTest::addColumn<int>("wordWidth");

        QTest::newRow("x86_64-apple-darwin") << int(Abi::X86Architecture)
                                             << int(Abi::DarwinOS) << int(Abi::GenericFlavor)
                                             << int(Abi::MachOFormat) << 64;

        QTest::newRow("x86_64-apple-darwin12.5.0") << int(Abi::X86Architecture)
                                                   << int(Abi::DarwinOS) << int(Abi::GenericFlavor)
                                                   << int(Abi::MachOFormat) << 64;

        QTest::newRow("x86_64-linux-gnu") << int(Abi::X86Architecture)
                                          << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                          << int(Abi::ElfFormat) << 64;

        QTest::newRow("x86_64-pc-mingw32msvc") << int(Abi::X86Architecture)
                                               << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                               << int(Abi::PEFormat) << 64;

        QTest::newRow("i586-pc-mingw32msvc") << int(Abi::X86Architecture)
                                             << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                             << int(Abi::PEFormat) << 32;

        QTest::newRow("i686-linux-gnu") << int(Abi::X86Architecture)
                                        << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                        << int(Abi::ElfFormat) << 32;

        QTest::newRow("i686-linux-android") << int(Abi::X86Architecture)
                                            << int(Abi::LinuxOS) << int(Abi::AndroidLinuxFlavor)
                                            << int(Abi::ElfFormat) << 32;

        QTest::newRow("i686-pc-linux-android") << int(Abi::X86Architecture)
                                               << int(Abi::LinuxOS) << int(Abi::AndroidLinuxFlavor)
                                               << int(Abi::ElfFormat) << 32;

        QTest::newRow("i686-pc-mingw32") << int(Abi::X86Architecture)
                                         << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                         << int(Abi::PEFormat) << 32;

        QTest::newRow("i686-w64-mingw32") << int(Abi::X86Architecture)
                                          << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                          << int(Abi::PEFormat) << 32;

        QTest::newRow("x86_64-pc-msys") << int(Abi::X86Architecture)
                                        << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                        << int(Abi::PEFormat) << 64;

        QTest::newRow("x86_64-pc-cygwin") << int(Abi::X86Architecture)
                                          << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                          << int(Abi::PEFormat) << 64;

        QTest::newRow("x86-pc-windows-msvc") << int(Abi::X86Architecture)
                                             << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                             << int(Abi::PEFormat) << 32;

        QTest::newRow("mingw32") << int(Abi::X86Architecture)
                                 << int(Abi::WindowsOS) << int(Abi::WindowsMSysFlavor)
                                 << int(Abi::PEFormat) << 0;

        QTest::newRow("arm-linux-android") << int(Abi::ArmArchitecture)
                                           << int(Abi::LinuxOS) << int(Abi::AndroidLinuxFlavor)
                                           << int(Abi::ElfFormat) << 32;

        QTest::newRow("arm-linux-androideabi") << int(Abi::ArmArchitecture)
                                               << int(Abi::LinuxOS) << int(Abi::AndroidLinuxFlavor)
                                               << int(Abi::ElfFormat) << 32;

        QTest::newRow("arm-none-linux-gnueabi") << int(Abi::ArmArchitecture)
                                                << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                                << int(Abi::ElfFormat) << 32;

        QTest::newRow("mips-linux-gnu") << int(Abi::MipsArchitecture)
                                        << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                        << int(Abi::ElfFormat) << 32;

        QTest::newRow("mips64-linux-octeon-gnu") << int(Abi::MipsArchitecture)
                                                 << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                                 << int(Abi::ElfFormat) << 64;

        QTest::newRow("mips64el-linux-gnuabi") << int(Abi::MipsArchitecture)
                                               << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                               << int(Abi::ElfFormat) << 64;

        QTest::newRow("arm-wrs-vxworks") << int(Abi::ArmArchitecture)
                                         << int(Abi::VxWorks) << int(Abi::VxWorksFlavor)
                                         << int(Abi::ElfFormat) << 32;

        QTest::newRow("x86_64-unknown-openbsd") << int(Abi::X86Architecture)
                                                << int(Abi::BsdOS) << int(Abi::OpenBsdFlavor)
                                                << int(Abi::ElfFormat) << 64;

        QTest::newRow("aarch64-unknown-linux-gnu") << int(Abi::ArmArchitecture)
                                                   << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                                   << int(Abi::ElfFormat) << 64;
        QTest::newRow("xtensa-lx106-elf")  << int(Abi::XtensaArchitecture)
                                          << int(Abi::BareMetalOS) << int(Abi::GenericFlavor)
                                          << int(Abi::ElfFormat) << 32;

        // Yes, that's the entire triplet
        QTest::newRow("avr") << int(Abi::AvrArchitecture)
                             << int(Abi::BareMetalOS) << int(Abi::GenericFlavor)
                             << int(Abi::ElfFormat) << 16;

        QTest::newRow("avr32") << int(Abi::Avr32Architecture)
                               << int(Abi::BareMetalOS) << int(Abi::GenericFlavor)
                               << int(Abi::ElfFormat) << 32;

        QTest::newRow("asmjs-unknown-emscripten") << int(Abi::AsmJsArchitecture)
                                                  << int(Abi::UnknownOS) << int(Abi::UnknownFlavor)
                                                  << int(Abi::EmscriptenFormat) << 32;

        QTest::newRow("loongarch64-linux-gnuabi") << int(Abi::LoongArchArchitecture)
                                                  << int(Abi::LinuxOS) << int(Abi::GenericFlavor)
                                                  << int(Abi::ElfFormat) << 64;
    }

    void testAbiFromTargetTriplet()
    {
        QFETCH(int, architecture);
        QFETCH(int, os);
        QFETCH(int, osFlavor);
        QFETCH(int, binaryFormat);
        QFETCH(int, wordWidth);

        const Abi expectedAbi = Abi(Abi::Architecture(architecture),
                                    Abi::OS(os), Abi::OSFlavor(osFlavor),
                                    Abi::BinaryFormat(binaryFormat),
                                    static_cast<unsigned char>(wordWidth));

        QCOMPARE(Abi::abiFromTargetTriplet(QLatin1String(QTest::currentDataTag())), expectedAbi);
    }

    void testUserOsFlavor_data()
    {
        QTest::addColumn<int>("os");
        QTest::addColumn<QString>("osFlavorName");
        QTest::addColumn<int>("expectedFlavor");

        QTest::newRow("linux-generic flavor")
            << int(Abi::LinuxOS) << "generic" << int(Abi::GenericFlavor);
        QTest::newRow("linux-unknown flavor")
            << int(Abi::LinuxOS) << "unknown" << int(Abi::UnknownFlavor);
        QTest::newRow("windows-msvc2010 flavor")
            << int(Abi::WindowsOS) << "msvc2010" << int(Abi::WindowsMsvc2010Flavor);
        QTest::newRow("windows-unknown flavor")
            << int(Abi::WindowsOS) << "unknown" << int(Abi::UnknownFlavor);

        QTest::newRow("windows-msvc2100 flavor")
            << int(Abi::WindowsOS) << "msvc2100" << int(Abi::UnknownFlavor) + 1;
        QTest::newRow("linux-msvc2100 flavor")
            << int(Abi::LinuxOS) << "msvc2100" << int(Abi::UnknownFlavor) + 1;

        QTest::newRow("windows-msvc2100 flavor reregister")
            << int(Abi::WindowsOS) << "msvc2100" << int(Abi::UnknownFlavor) + 1;
        QTest::newRow("linux-msvc2100 flavor reregister")
            << int(Abi::LinuxOS) << "msvc2100" << int(Abi::UnknownFlavor) + 1;
        QTest::newRow("unix-msvc2100 flavor register")
            << int(Abi::UnixOS) << "msvc2100" << int(Abi::UnknownFlavor) + 1;
    }

    void testUserOsFlavor()
    {
        QFETCH(int, os);
        QFETCH(QString, osFlavorName);
        QFETCH(int, expectedFlavor);

        QMap<int, QList<Abi::OSFlavor>> osFlavorMap;
        for (int i = 0; i <= Abi::UnknownOS; ++i)
            osFlavorMap.insert(i, Abi::flavorsForOs(static_cast<ProjectExplorer::Abi::OS>(i)));

        Abi::OSFlavor osFlavor = Abi::registerOsFlavor({static_cast<Abi::OS>(os)}, osFlavorName);
        QCOMPARE(osFlavor, static_cast<Abi::OSFlavor>(expectedFlavor));

        for (int i = 0; i <= Abi::UnknownOS; ++i) {
            const QList<Abi::OSFlavor> flavors = Abi::flavorsForOs(static_cast<Abi::OS>(i));
            const QList<Abi::OSFlavor> previousFlavors = osFlavorMap.value(static_cast<Abi::OS>(i));
            const int previousCount = previousFlavors.count();

            if (i == os && previousCount != flavors.count()) {
                QVERIFY(flavors.count() == previousCount + 1);
                QVERIFY(flavors.contains(osFlavor));
                for (const Abi::OSFlavor &f : previousFlavors) {
                    if (f == osFlavor)
                        continue;
                    QVERIFY(previousFlavors.contains(f));
                }
            } else {
                QCOMPARE(flavors, previousFlavors);
            }
        }
    }
};

QObject *createAbiTest()
{
    return new AbiTest;
}

} // namespace Internal

#endif // WITH_TESTS

} // namespace ProjectExplorer

#ifdef WITH_TESTS
#include <abi.moc>
#endif
