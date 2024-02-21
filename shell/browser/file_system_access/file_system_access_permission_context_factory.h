// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_FILE_SYSTEM_ACCESS_FILE_SYSTEM_ACCESS_PERMISSION_CONTEXT_FACTORY_H_
#define ELECTRON_SHELL_BROWSER_FILE_SYSTEM_ACCESS_FILE_SYSTEM_ACCESS_PERMISSION_CONTEXT_FACTORY_H_

#include "base/no_destructor.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"

class ElectronFileSystemAccessPermissionContext;

class FileSystemAccessPermissionContextFactory
    : public BrowserContextKeyedServiceFactory {
 public:
  static ElectronFileSystemAccessPermissionContext* GetForBrowserContext(
      content::BrowserContext* context);
  static FileSystemAccessPermissionContextFactory* GetInstance();

  FileSystemAccessPermissionContextFactory(
      const FileSystemAccessPermissionContextFactory&) = delete;
  FileSystemAccessPermissionContextFactory& operator=(
      const FileSystemAccessPermissionContextFactory&) = delete;

 private:
  friend class base::NoDestructor<FileSystemAccessPermissionContextFactory>;

  FileSystemAccessPermissionContextFactory();
  ~FileSystemAccessPermissionContextFactory() override;

  // BrowserContextKeyedServiceFactory:
  KeyedService* BuildServiceInstanceFor(
      content::BrowserContext* context) const override;
};

#endif  // ELECTRON_SHELL_BROWSER_FILE_SYSTEM_ACCESS_FILE_SYSTEM_ACCESS_PERMISSION_CONTEXT_FACTORY_H_