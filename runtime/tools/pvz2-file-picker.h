/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PVZ2_FILE_PICKER_H
#define PVZ2_FILE_PICKER_H

/*
 * The host side of the game's Import/Export menu.
 *
 * The driver turns FilePickerHelper's five static methods into one request for a
 * path (see husk-tl-pvz2.h); this file presents the platform's own picker and
 * answers with the chosen path.  Nothing here runs unless the game asks: the
 * picker is built in every app, so a normal player gets the real panel.
 */
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#else
#import <AppKit/AppKit.h>
#endif

#include "husk-tl-pvz2.h"
#include "pvz2-profile.h"

void tl_log_line(const char *fmt, ...);

#if TARGET_OS_IPHONE
@interface PVZ2FilePicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, weak) UIViewController *root;
@property(nonatomic) int kind;
@property(nonatomic, copy) NSString *prepared;
@end

@implementation PVZ2FilePicker

- (void)openKind:(int)kind internal:(NSString *)internal suggested:(NSString *)suggested prepared:(NSString *)prepared
{
    (void)internal;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.root) {
            tl_log_line("pvz2: file picker: no view controller to present from");
            tl_pvz2_file_picker_answer(NULL);
            return;
        }
        self.kind = kind;
        self.prepared = prepared;
        UIDocumentPickerViewController *picker;
        if (kind == TL_PVZ2_PICK_IMPORT_FILE || kind == TL_PVZ2_PICK_IMPORT_TO_DIR) {
            picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
            picker.allowsMultipleSelection = NO;
            tl_log_line("pvz2: file picker: presenting the document picker to import a file");
        } else {
            NSURL *url = [NSURL fileURLWithPath:prepared];
            picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[url] asCopy:YES];
            tl_log_line("pvz2: file picker: presenting the document picker to export %s",
                        suggested.UTF8String ?: "");
        }
        picker.delegate = self;
        [self.root presentViewController:picker animated:YES completion:nil];
    });
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    (void)controller;
    if (self.kind == TL_PVZ2_PICK_IMPORT_FILE || self.kind == TL_PVZ2_PICK_IMPORT_TO_DIR) {
        NSString *path = urls.firstObject.path;
        tl_log_line("pvz2: file picker: the document picker chose %s", path.UTF8String ?: "(nothing)");
        tl_pvz2_file_picker_answer(path.UTF8String);
    } else {
        tl_log_line("pvz2: file picker: the document picker exported %s",
                    self.prepared.UTF8String ?: "(nothing)");
        /* The export picker has already copied the prepared file.  Answering
         * with that path tells the driver there is nothing left to copy. */
        tl_pvz2_file_picker_answer(self.prepared.UTF8String);
    }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    (void)controller;
    tl_log_line("pvz2: file picker: the document picker was cancelled");
    tl_pvz2_file_picker_answer(NULL);
}
@end

static PVZ2FilePicker *pvz2_file_picker;

static void pvz2_file_picker_open(int kind, const char *internal, const char *suggested, const char *prepared)
{
    [pvz2_file_picker openKind:kind
                       internal:[NSString stringWithUTF8String:internal ?: ""]
                      suggested:[NSString stringWithUTF8String:suggested ?: ""]
                       prepared:[NSString stringWithUTF8String:prepared ?: ""]];
}

static void pvz2_file_picker_install(UIViewController *root)
{
    pvz2_file_picker = [PVZ2FilePicker new];
    pvz2_file_picker.root = root;
    static const tl_pvz2_file_picker_hooks hooks = { .open = pvz2_file_picker_open };
    tl_pvz2_set_file_picker_hooks(&hooks);
}

#else

@interface PVZ2FilePicker : NSObject
@property(nonatomic, weak) NSWindow *window;
@end

@implementation PVZ2FilePicker

- (void)openKind:(int)kind internal:(NSString *)internal suggested:(NSString *)suggested prepared:(NSString *)prepared
{
    (void)internal;
    (void)prepared;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.window) {
            tl_log_line("pvz2: file picker: no window to attach the panel to");
            tl_pvz2_file_picker_answer(NULL);
            return;
        }
        if (kind == TL_PVZ2_PICK_IMPORT_FILE || kind == TL_PVZ2_PICK_IMPORT_TO_DIR) {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.title = [NSString stringWithFormat:@"Import into %@", @(PVZ2_PROFILE_APP_NAME)];
            panel.prompt = @"Import";
            panel.message = [NSString stringWithFormat:@"Choose the file to import as %@.", suggested];
            tl_log_line("pvz2: file picker: showing the open panel over the game");
            [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
                NSString *path = response == NSModalResponseOK ? panel.URL.path : nil;
                tl_log_line("pvz2: file picker: the open panel %s",
                            path ? path.UTF8String : "was cancelled");
                tl_pvz2_file_picker_answer(path.UTF8String);
            }];
        } else {
            NSSavePanel *panel = [NSSavePanel savePanel];
            panel.canCreateDirectories = YES;
            panel.nameFieldStringValue = suggested;
            panel.title = [NSString stringWithFormat:@"Export from %@", @(PVZ2_PROFILE_APP_NAME)];
            panel.prompt = @"Export";
            tl_log_line("pvz2: file picker: showing the save panel over the game as %s",
                        suggested.UTF8String ?: "");
            [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
                NSString *path = response == NSModalResponseOK ? panel.URL.path : nil;
                tl_log_line("pvz2: file picker: the save panel %s",
                            path ? path.UTF8String : "was cancelled");
                tl_pvz2_file_picker_answer(path.UTF8String);
            }];
        }
    });
}

@end

static PVZ2FilePicker *pvz2_file_picker;

static void pvz2_file_picker_open(int kind, const char *internal, const char *suggested, const char *prepared)
{
    [pvz2_file_picker openKind:kind
                       internal:[NSString stringWithUTF8String:internal ?: ""]
                      suggested:[NSString stringWithUTF8String:suggested ?: ""]
                       prepared:[NSString stringWithUTF8String:prepared ?: ""]];
}

static void pvz2_file_picker_install(NSWindow *window)
{
    pvz2_file_picker = [PVZ2FilePicker new];
    pvz2_file_picker.window = window;
    static const tl_pvz2_file_picker_hooks hooks = { .open = pvz2_file_picker_open };
    tl_pvz2_set_file_picker_hooks(&hooks);
}

#endif

#endif
