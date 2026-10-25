/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PVZ2_NOTIFICATIONS_H
#define PVZ2_NOTIFICATIONS_H

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#import <UserNotifications/UserNotifications.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

#include "husk-tl-pvz2.h"

void tl_log_line(const char *fmt, ...);

@interface PVZ2NotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

static void pvz2_capture_notification_fields(NSDictionary *fields)
{
    tl_pvz2_clear_intent_extra_data();
    [fields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL *stop) {
        (void)stop;
        if ([key isKindOfClass:NSString.class] && [value isKindOfClass:NSString.class])
            tl_pvz2_set_intent_extra_data([key UTF8String], [value UTF8String]);
    }];
}

static void pvz2_capture_notification_response(UNNotificationResponse *response)
{
    pvz2_capture_notification_fields(response.notification.request.content.userInfo);
}

static void pvz2_notifications_app_became_active(void);

@implementation PVZ2NotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler
{
    (void)center; (void)notification;
    completionHandler(UNNotificationPresentationOptionAlert |
                      UNNotificationPresentationOptionSound |
                      UNNotificationPresentationOptionBadge);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
 didReceiveNotificationResponse:(UNNotificationResponse *)response
          withCompletionHandler:(void (^)(void))completionHandler
{
    (void)center;
    pvz2_capture_notification_response(response);
    tl_log_line("pvz2: opened local reminder; its data.fields are available to the game");
    completionHandler();
}
@end

static dispatch_queue_t pvz2_notification_queue;
static NSMutableDictionary<NSNumber *, NSDictionary *> *pvz2_pending_reminders;
static NSMutableDictionary<NSString *, NSString *> *pvz2_channel_sounds;
static NSMutableSet<NSNumber *> *pvz2_logged_dropped_repeats;
static PVZ2NotificationDelegate *pvz2_notification_delegate;
static BOOL pvz2_permission_requested, pvz2_permission_status_loaded;
static BOOL pvz2_permission_resolved, pvz2_permission_granted;
/* The game wants notifications: it created its channels at launch (Android 13 asks right then for
 * apps like this one) or scheduled a reminder. */
static BOOL pvz2_permission_wanted;
static NSUInteger pvz2_adds_in_flight;
#if TARGET_OS_IPHONE
static UIBackgroundTaskIdentifier pvz2_background_task;
#endif

static void pvz2_drain_reminders(void);
static void pvz2_request_authorization_if_needed(void);

static void pvz2_notifications_init(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        pvz2_notification_queue = dispatch_queue_create("io.reflourished.pvz2.notifications", DISPATCH_QUEUE_SERIAL);
        pvz2_pending_reminders = [NSMutableDictionary dictionary];
        pvz2_channel_sounds = [NSMutableDictionary dictionary];
        pvz2_logged_dropped_repeats = [NSMutableSet set];
#if TARGET_OS_IPHONE
        pvz2_background_task = UIBackgroundTaskInvalid;
#endif
        pvz2_notification_delegate = [PVZ2NotificationDelegate new];
        UNUserNotificationCenter.currentNotificationCenter.delegate = pvz2_notification_delegate;
        [UNUserNotificationCenter.currentNotificationCenter getNotificationSettingsWithCompletionHandler:
            ^(UNNotificationSettings *settings) {
                dispatch_async(pvz2_notification_queue, ^{
                    UNAuthorizationStatus status = settings.authorizationStatus;
                    BOOL granted = status == UNAuthorizationStatusAuthorized ||
                                   status == UNAuthorizationStatusProvisional;
#if TARGET_OS_IPHONE
                    if (@available(iOS 14.0, *))
                        granted = granted || status == UNAuthorizationStatusEphemeral;
#endif
                    pvz2_permission_status_loaded = YES;
                    if (granted) {
                        pvz2_permission_resolved = YES;
                        pvz2_permission_granted = YES;
                        tl_log_line("pvz2: notification permission already granted at launch");
                        pvz2_drain_reminders();
                    } else if (status == UNAuthorizationStatusDenied) {
                        pvz2_permission_resolved = YES;
                        pvz2_permission_granted = NO;
                        tl_log_line("pvz2: notification permission was denied before launch");
                        pvz2_drain_reminders();
                    } else if (status == UNAuthorizationStatusNotDetermined) {
                        tl_log_line("pvz2: notification permission not asked yet");
                        pvz2_request_authorization_if_needed();
                    } else {
                        pvz2_permission_resolved = YES;
                        pvz2_permission_granted = NO;
                        tl_log_line("pvz2: notification permission has an unsupported status (%ld); reminders will not be scheduled",
                                    (long)status);
                        pvz2_drain_reminders();
                    }
                });
            }];
    });
}

static NSString *pvz2_string(const char *s)
{
    return s ? [NSString stringWithUTF8String:s] : nil;
}

static NSString *pvz2_request_id(NSInteger identifier, NSString *kind)
{
    return [NSString stringWithFormat:@"pvz2.reminder.%ld.%@", (long)identifier, kind];
}

static UNNotificationTrigger *pvz2_absolute_trigger(int64_t fireTimeMs)
{
    NSDate *date = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)fireTimeMs / 1000.0];
    NSCalendar *calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
    NSTimeZone *utc = [NSTimeZone timeZoneForSecondsFromGMT:0];
    calendar.timeZone = utc;
    NSDateComponents *parts = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth |
        NSCalendarUnitDay | NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond
        fromDate:date];
    parts.calendar = calendar;
    parts.timeZone = utc;
    return [UNCalendarNotificationTrigger triggerWithDateMatchingComponents:parts repeats:NO];
}

static UNMutableNotificationContent *pvz2_content(NSDictionary *reminder)
{
    UNMutableNotificationContent *content = [UNMutableNotificationContent new];
    content.title = reminder[@"title"] ?: @"";
    content.body = reminder[@"body"] ?: @"";
    content.threadIdentifier = [NSString stringWithFormat:@"PVZ2.%@", reminder[@"group"] ?: @0];
    id userInfo = reminder[@"userInfo"];
    content.userInfo = [userInfo isKindOfClass:NSString.class] ? @{ @"userInfo": userInfo, @"notificationType": @"LN" } :
                                  @{ @"notificationType": @"LN" };

    NSString *channel = reminder[@"channel"];
    NSString *sound = channel.length ? pvz2_channel_sounds[channel] : nil;
    if (!sound) sound = reminder[@"sound"];
    if (sound.length) {
        if (!sound.pathExtension.length) sound = [sound stringByAppendingPathExtension:@"caf"];
        content.sound = [UNNotificationSound soundNamed:sound];
    } else {
        content.sound = UNNotificationSound.defaultSound;
    }
    return content;
}

static void pvz2_add_request(NSDictionary *reminder, NSString *kind, UNNotificationTrigger *trigger)
{
    NSInteger identifier = [reminder[@"id"] integerValue];
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:pvz2_request_id(identifier, kind)
        content:pvz2_content(reminder) trigger:trigger];
    pvz2_adds_in_flight++;
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request
        withCompletionHandler:^(NSError *error) {
            dispatch_async(pvz2_notification_queue, ^{
                if (error) tl_log_line("pvz2: reminder %ld failed to reach UserNotifications (%s)",
                                       (long)identifier, error.localizedDescription.UTF8String);
                if (pvz2_adds_in_flight) pvz2_adds_in_flight--;
#if TARGET_OS_IPHONE
                if (!pvz2_adds_in_flight && pvz2_background_task != UIBackgroundTaskInvalid) {
                    UIBackgroundTaskIdentifier task = pvz2_background_task;
                    pvz2_background_task = UIBackgroundTaskInvalid;
                    dispatch_sync(dispatch_get_main_queue(), ^{
                        [UIApplication.sharedApplication endBackgroundTask:task];
                    });
                    tl_log_line("pvz2: background notification scheduling finished");
                }
#endif
            });
        }];
}

static void pvz2_begin_background_task_if_needed(void)
{
#if TARGET_OS_IPHONE
    if (pvz2_background_task != UIBackgroundTaskInvalid) return;
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    dispatch_sync(dispatch_get_main_queue(), ^{
        UIApplication *application = UIApplication.sharedApplication;
        task = [application beginBackgroundTaskWithName:@"PVZ2 reminder scheduling"
            expirationHandler:^{
                dispatch_async(pvz2_notification_queue, ^{
                    if (pvz2_background_task != task) return;
                    pvz2_background_task = UIBackgroundTaskInvalid;
                    dispatch_sync(dispatch_get_main_queue(), ^{
                        [UIApplication.sharedApplication endBackgroundTask:task];
                    });
                    tl_log_line("pvz2: background notification scheduling time expired");
                });
            }];
    });
    pvz2_background_task = task;
    if (task == UIBackgroundTaskInvalid)
        tl_log_line("pvz2: could not begin a background task for notification scheduling");
#endif
}

static void pvz2_schedule_authorized(NSDictionary *reminder)
{
    NSInteger identifier = [reminder[@"id"] integerValue];
    int64_t fireTimeMs = [reminder[@"fireTimeMs"] longLongValue];
    NSInteger interval = [reminder[@"interval"] integerValue];
    NSString *onceID = pvz2_request_id(identifier, @"first");
    NSString *repeatID = pvz2_request_id(identifier, @"repeat");
    [UNUserNotificationCenter.currentNotificationCenter removePendingNotificationRequestsWithIdentifiers:@[onceID, repeatID]];

    NSTimeInterval delay = ((NSTimeInterval)fireTimeMs / 1000.0) - NSDate.date.timeIntervalSince1970;
    UNNotificationTrigger *first = delay < 1.0 ?
        [UNTimeIntervalNotificationTrigger triggerWithTimeInterval:1 repeats:NO] : pvz2_absolute_trigger(fireTimeMs);
    pvz2_begin_background_task_if_needed();
    pvz2_add_request(reminder, @"first", first);

    if (interval > 0) {
        NSNumber *key = @(identifier);
        if (![pvz2_logged_dropped_repeats containsObject:key]) {
            [pvz2_logged_dropped_repeats addObject:key];
            tl_log_line("pvz2: reminder %ld repeat interval %ld s was dropped; UserNotifications cannot repeat from this absolute first-fire time",
                        (long)identifier, (long)interval);
        }
    }
}

/* TL_PVZ2_NOTIFY=soon: one extra reminder a minute after permission is known, so delivery and the
 * tap back into the game can be seen on a device whatever the profile has to remind about. */
static void pvz2_schedule_test_reminder_once(void)
{
    static BOOL done;
    const char *mode = getenv("TL_PVZ2_NOTIFY");
    if (done || !mode || strcmp(mode, "soon")) return;
    done = YES;
    NSDictionary *test = @{ @"id": @(-1), @"group": @0,
        @"fireTimeMs": @((int64_t)((NSDate.date.timeIntervalSince1970 + 60) * 1000)), @"interval": @0,
        @"title": @"Plants vs Zombies 2", @"body": @"Test reminder (TL_PVZ2_NOTIFY=soon)",
        @"sound": @"", @"channel": @"", @"userInfo": [NSNull null] };
    pvz2_schedule_authorized(test);
    tl_log_line("pvz2: test reminder scheduled a minute from now");
}

static void pvz2_drain_reminders(void)
{
    if (!pvz2_permission_resolved) return;
    NSArray<NSDictionary *> *scheduled = pvz2_pending_reminders.allValues;
    [pvz2_pending_reminders removeAllObjects];
    if (!pvz2_permission_granted) {
        for (NSDictionary *r in scheduled)
            tl_log_line("pvz2: reminder %ld not scheduled (notification permission denied)", (long)[r[@"id"] integerValue]);
        return;
    }
    for (NSDictionary *r in scheduled) pvz2_schedule_authorized(r);
    pvz2_schedule_test_reminder_once();
}

static BOOL pvz2_application_is_active(void)
{
#if TARGET_OS_IPHONE
    return UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
#else
    return NSApp.isActive;
#endif
}

static void pvz2_request_authorization_if_needed(void)
{
    const char *mode = getenv("TL_PVZ2_NOTIFY");
    if (mode && !strcmp(mode, "log")) return;  /* tests: never a prompt on the tester's screen */
    if (!pvz2_permission_status_loaded || pvz2_permission_resolved || pvz2_permission_requested ||
        !pvz2_permission_wanted ||
        !pvz2_application_is_active()) return;
    pvz2_permission_requested = YES;
    tl_log_line("pvz2: asking for notification permission");
    [UNUserNotificationCenter.currentNotificationCenter requestAuthorizationWithOptions:
        (UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
        completionHandler:^(BOOL granted, NSError *error) {
            dispatch_async(pvz2_notification_queue, ^{
                pvz2_permission_resolved = YES;
                pvz2_permission_granted = granted && !error;
                tl_log_line("pvz2: notification permission %s", pvz2_permission_granted ? "granted" : "refused");
                if (error) tl_log_line("pvz2: notification authorization failed (%s)", error.localizedDescription.UTF8String);
                pvz2_drain_reminders();
            });
        }];
}

static void pvz2_schedule_reminder(const tl_pvz2_reminder *reminder)
{
    NSDictionary *item = @{
        @"id": @(reminder->identifier), @"group": @(reminder->group_identifier),
        @"fireTimeMs": @(reminder->fire_time_ms), @"interval": @(reminder->seconds_interval),
        @"title": pvz2_string(reminder->title) ?: @"", @"body": pvz2_string(reminder->body) ?: @"",
        @"sound": pvz2_string(reminder->sound) ?: @"", @"channel": pvz2_string(reminder->channel_id) ?: @"",
        @"userInfo": pvz2_string(reminder->user_info) ?: [NSNull null],
    };
    dispatch_async(pvz2_notification_queue, ^{
        NSNumber *key = item[@"id"];
        pvz2_pending_reminders[key] = item;
        pvz2_permission_wanted = YES;
        if (pvz2_permission_resolved) { pvz2_drain_reminders(); return; }
        pvz2_request_authorization_if_needed();
    });
}

static void pvz2_notifications_app_became_active(void)
{
    dispatch_async(pvz2_notification_queue, ^{ pvz2_request_authorization_if_needed(); });
}

static void pvz2_cancel_reminder(int identifier)
{
    dispatch_async(pvz2_notification_queue, ^{
        [pvz2_pending_reminders removeObjectForKey:@(identifier)];
        [UNUserNotificationCenter.currentNotificationCenter removePendingNotificationRequestsWithIdentifiers:@[
            pvz2_request_id(identifier, @"first"), pvz2_request_id(identifier, @"repeat")]];
    });
}

static void pvz2_cancel_all_reminders(const int *identifiers, int count)
{
    NSMutableArray<NSNumber *> *keys = [NSMutableArray arrayWithCapacity:(NSUInteger)count];
    NSMutableArray<NSString *> *requestIDs = [NSMutableArray arrayWithCapacity:(NSUInteger)count * 2];
    for (int i = 0; i < count; i++) {
        NSNumber *key = @(identifiers[i]);
        [keys addObject:key];
        [requestIDs addObject:pvz2_request_id(identifiers[i], @"first")];
        [requestIDs addObject:pvz2_request_id(identifiers[i], @"repeat")];
    }
    NSArray<NSNumber *> *savedKeys = [keys copy];
    NSArray<NSString *> *savedRequestIDs = [requestIDs copy];
    dispatch_async(pvz2_notification_queue, ^{
        [pvz2_pending_reminders removeObjectsForKeys:savedKeys];
        [UNUserNotificationCenter.currentNotificationCenter removePendingNotificationRequestsWithIdentifiers:savedRequestIDs];
    });
}

static void pvz2_create_notification_channel(const char *identifier, const char *name,
                                             const char *description, const char *sound)
{
    (void)name; (void)description;
    NSString *key = pvz2_string(identifier) ?: @"";
    NSString *value = pvz2_string(sound) ?: @"";
    dispatch_async(pvz2_notification_queue, ^{
        pvz2_channel_sounds[key] = value;
        pvz2_permission_wanted = YES;
        pvz2_request_authorization_if_needed();
    });
}

static void pvz2_delete_notification_channel(const char *identifier)
{
    NSString *key = pvz2_string(identifier) ?: @"";
    dispatch_async(pvz2_notification_queue, ^{ [pvz2_channel_sounds removeObjectForKey:key]; });
}

static const char *pvz2_notification_timezone(void)
{
    static char name[128];
    NSString *timezone = NSTimeZone.localTimeZone.name;
    snprintf(name, sizeof(name), "%s", timezone.UTF8String ?: "UTC");
    return name;
}

static void pvz2_notifications_install(void)
{
    pvz2_notifications_init();
}

static const tl_pvz2_notification_hooks pvz2_notification_hooks = {
    .schedule = pvz2_schedule_reminder,
    .cancel = pvz2_cancel_reminder,
    .cancel_all = pvz2_cancel_all_reminders,
    .create_channel = pvz2_create_notification_channel,
    .delete_channel = pvz2_delete_notification_channel,
    .timezone = pvz2_notification_timezone,
};

#endif
