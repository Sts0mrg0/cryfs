#include "CryConfigLoader.h"
#include "CryConfigFile.h"
#include <boost/filesystem.hpp>
#include <cpp-utils/random/Random.h>
#include <cpp-utils/logging/logging.h>
#include <boost/algorithm/string/predicate.hpp>
#include <gitversion/gitversion.h>
#include <gitversion/VersionCompare.h>
#include "cryfs/impl/localstate/LocalStateDir.h"
#include "cryfs/impl/localstate/LocalStateMetadata.h"
#include "cryfs/impl/CryfsException.h"

namespace bf = boost::filesystem;
using cpputils::Console;
using cpputils::RandomGenerator;
using cpputils::unique_ref;
using boost::optional;
using boost::none;
using std::shared_ptr;
using std::string;
using std::shared_ptr;
using gitversion::VersionCompare;
using namespace cpputils::logging;

namespace cryfs {

CryConfigLoader::CryConfigLoader(shared_ptr<Console> console, RandomGenerator &keyGenerator, unique_ref<CryKeyProvider> keyProvider, LocalStateDir localStateDir, const optional<string> &cipherFromCommandLine, const boost::optional<uint32_t> &blocksizeBytesFromCommandLine, const boost::optional<bool> &missingBlockIsIntegrityViolationFromCommandLine)
    : _console(console), _creator(std::move(console), keyGenerator, localStateDir), _keyProvider(std::move(keyProvider)),
      _cipherFromCommandLine(cipherFromCommandLine), _blocksizeBytesFromCommandLine(blocksizeBytesFromCommandLine),
      _missingBlockIsIntegrityViolationFromCommandLine(missingBlockIsIntegrityViolationFromCommandLine),
      _localStateDir(std::move(localStateDir)) {
}

optional<CryConfigLoader::ConfigLoadResult> CryConfigLoader::_loadConfig(bf::path filename, bool allowFilesystemUpgrade, bool allowReplacedFilesystem) {
  auto config = CryConfigFile::load(std::move(filename), _keyProvider.get());
  if (config.is_left()) {
    return none;
  }
#ifndef CRYFS_NO_COMPATIBILITY
  //Since 0.9.7 and 0.9.8 set their own version to cryfs.version instead of the filesystem format version (which is 0.9.6), overwrite it
  if (config.right()->config()->Version() == "0.9.7" || config.right()->config()->Version() == "0.9.8") {
    config.right()->config()->SetVersion("0.9.6");
  }
#endif
  _checkVersion(*config.right()->config(), allowFilesystemUpgrade);
#ifndef CRYFS_NO_COMPATIBILITY
  //Since 0.9.3-alpha set the config value cryfs.blocksizeBytes wrongly to 32768 (but didn't use the value), we have to fix this here.
  if (config.right()->config()->Version() != "0+unknown" && VersionCompare::isOlderThan(config.right()->config()->Version(), "0.9.3-rc1")) {
    config.right()->config()->SetBlocksizeBytes(32832);
  }
#endif
  if (config.right()->config()->Version() != CryConfig::FilesystemFormatVersion) {
    config.right()->config()->SetVersion(CryConfig::FilesystemFormatVersion);
    config.right()->save();
  }
  if (config.right()->config()->LastOpenedWithVersion() != gitversion::VersionString()) {
    config.right()->config()->SetLastOpenedWithVersion(gitversion::VersionString());
    config.right()->save();
  }
  _checkCipher(*config.right()->config());
  auto localState = LocalStateMetadata::loadOrGenerate(_localStateDir.forFilesystemId(config.right()->config()->FilesystemId()), cpputils::Data::FromString(config.right()->config()->EncryptionKey()), allowReplacedFilesystem);
  uint32_t myClientId = localState.myClientId();
  _checkMissingBlocksAreIntegrityViolations(config.right().get(), myClientId);
  return ConfigLoadResult {std::move(config.right()), myClientId};
}

void CryConfigLoader::_checkVersion(const CryConfig &config, bool allowFilesystemUpgrade) {
  if (gitversion::VersionCompare::isOlderThan(CryConfig::FilesystemFormatVersion, config.Version())) {
    if (!_console->askYesNo("This filesystem is for CryFS " + config.Version() + " or later and should not be opened with older versions. It is strongly recommended to update your CryFS version. However, if you have backed up your base directory and know what you're doing, you can continue trying to load it. Do you want to continue?", false)) {
      throw CryfsException("This filesystem is for CryFS " + config.Version() + " or later. Please update your CryFS version.", ErrorCode::TooNewFilesystemFormat);
    }
  }
  if (!allowFilesystemUpgrade && gitversion::VersionCompare::isOlderThan(config.Version(), CryConfig::FilesystemFormatVersion)) {
    if (!_console->askYesNo("This filesystem is for CryFS " + config.Version() + " (or a later version with the same storage format). You're running a CryFS version using storage format " + CryConfig::FilesystemFormatVersion + ". It is recommended to create a new filesystem with CryFS 0.10 and copy your files into it. If you don't want to do that, we can also attempt to migrate the existing filesystem, but that can take a long time, you won't be getting some of the performance advantages of the 0.10 release series, and if the migration fails, your data may be lost. If you decide to continue, please make sure you have a backup of your data. Do you want to attempt a migration now?", false)) {
      throw CryfsException("This filesystem is for CryFS " + config.Version() + " (or a later version with the same storage format). It has to be migrated.", ErrorCode::TooOldFilesystemFormat);
    }
  }
}

void CryConfigLoader::_checkCipher(const CryConfig &config) const {
  if (_cipherFromCommandLine != none && config.Cipher() != *_cipherFromCommandLine) {
    throw CryfsException(string() + "Filesystem uses " + config.Cipher() + " cipher and not " + *_cipherFromCommandLine + " as specified.", ErrorCode::WrongCipher);
  }
}

void CryConfigLoader::_checkMissingBlocksAreIntegrityViolations(CryConfigFile *configFile, uint32_t myClientId) {
  if (_missingBlockIsIntegrityViolationFromCommandLine == optional<bool>(true) && configFile->config()->ExclusiveClientId() == none) {
    throw CryfsException("You specified on the command line to treat missing blocks as integrity violations, but the file system is not setup to do that.", ErrorCode::FilesystemHasDifferentIntegritySetup);
  }
  if (_missingBlockIsIntegrityViolationFromCommandLine == optional<bool>(false) && configFile->config()->ExclusiveClientId() != none) {
    throw CryfsException("You specified on the command line to not treat missing blocks as integrity violations, but the file system is setup to do that.", ErrorCode::FilesystemHasDifferentIntegritySetup);
  }

  // If the file system is set up to treat missing blocks as integrity violations, but we're accessing from a different client, ask whether they want to disable the feature.
  auto exclusiveClientId = configFile->config()->ExclusiveClientId();
  if (exclusiveClientId != none && *exclusiveClientId != myClientId) {
    if (!_console->askYesNo("\nThis filesystem is setup to treat missing blocks as integrity violations and therefore only works in single-client mode. You are trying to access it from a different client.\nDo you want to disable this integrity feature and stop treating missing blocks as integrity violations?\nChoosing yes will not affect the confidentiality of your data, but in future you might not notice if an attacker deletes one of your files.", false)) {
      throw CryfsException("File system is in single-client mode and can only be used from the client that created it.", ErrorCode::SingleClientFileSystem);
    }
    configFile->config()->SetExclusiveClientId(none);
    configFile->save();
  }
}

optional<CryConfigLoader::ConfigLoadResult> CryConfigLoader::load(bf::path filename, bool allowFilesystemUpgrade, bool allowReplacedFilesystem) {
  return _loadConfig(std::move(filename), allowFilesystemUpgrade, allowReplacedFilesystem);
}

optional<CryConfigLoader::ConfigLoadResult> CryConfigLoader::loadOrCreate(bf::path filename, bool allowFilesystemUpgrade, bool allowReplacedFilesystem) {
  if (bf::exists(filename)) {
    return _loadConfig(std::move(filename), allowFilesystemUpgrade, allowReplacedFilesystem);
  } else {
    return _createConfig(std::move(filename), allowReplacedFilesystem);
  }
}

CryConfigLoader::ConfigLoadResult CryConfigLoader::_createConfig(bf::path filename, bool allowReplacedFilesystem) {
  auto config = _creator.create(_cipherFromCommandLine, _blocksizeBytesFromCommandLine, _missingBlockIsIntegrityViolationFromCommandLine, allowReplacedFilesystem);
  auto result = CryConfigFile::create(std::move(filename), std::move(config.config), _keyProvider.get());
  return ConfigLoadResult {std::move(result), config.myClientId};
}


}
