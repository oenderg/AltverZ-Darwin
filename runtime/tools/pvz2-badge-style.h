/* Shared in-level badge styling. The layer coordinates are the Metal layer's
 * own coordinates, so the game's picture and its labels transform together. */
#import <CoreText/CoreText.h>
#import <QuartzCore/QuartzCore.h>

static CTFontRef pvz2_badge_font(CGFloat size)
{
    static bool registered;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        CFTimeInterval start = CACurrentMediaTime();
        NSURL *url = [NSBundle.mainBundle URLForResource:@"level-ui" withExtension:@"ttf"];
        CFErrorRef error = NULL;
        registered = url && CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,
                                                               kCTFontManagerScopeProcess, &error);
        tl_log_line("badges: font %s in %.2f ms%s", registered ? "fbUsv8C5eI-Black" : "bold system fallback",
                    (CACurrentMediaTime() - start) * 1000, error ? ": registration failed" : "");
        if (error) CFRelease(error);
    });
    CGAffineTransform italic = CGAffineTransformMake(1, 0, 0.18, 1, 0, 0);
    if (registered) return CTFontCreateWithName(CFSTR("fbUsv8C5eI-Black"), size, &italic);
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, NULL);
    CTFontRef font = CTFontCreateCopyWithAttributes(base, size, &italic, NULL);
    CFRelease(base);
    return font;
}

static const int *pvz2_badge_rect(int index, const tl_pvz2_badge_snapshot *s)
{
    if (index < TL_PVZ2_BADGE_SLOTS) return index < s->slot_count ? s->packet[index] : NULL;
    index -= TL_PVZ2_BADGE_SLOTS;
    if (index == 0) return s->shovel;
    if (index == 1) return s->fast_forward;
    if (index == 2) return s->plant_food;
    index -= 3;
    return index >= 0 && index < s->powerup_count ? s->powerup[index] : NULL;
}

/* Register before a live board appears, rather than on its first visible frame. */
static void pvz2_badge_prepare(void)
{
    CTFontRef font = pvz2_badge_font(20);
    CFRelease(font);
}

/* Prepare at one size so a control can scale without rebuilding the text.
 * CoreText supplies fallback glyphs when a key is absent from the subset. */
static void pvz2_badge_prime(CATextLayer *text, NSString *label)
{
    CATextLayer *back = (CATextLayer *)text.superlayer.sublayers.firstObject;
    NSAttributedString *old = [text.string isKindOfClass:NSAttributedString.class] ? text.string : nil;
    if (!label || [old.string isEqualToString:label]) return;
    const CGFloat fontSize = 32;
    CTFontRef font = pvz2_badge_font(fontSize);
    static CGColorRef yellow, outline;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGFloat y[] = {1.0, 0.94, 0.18, 1};
        CGFloat o[] = {0.08, 0.06, 0.02, 1};
        yellow = CGColorCreate(space, y);
        outline = CGColorCreate(space, o);
        CGColorSpaceRelease(space);
    });
    NSDictionary *attrs = @{(__bridge id)kCTFontAttributeName: (__bridge id)font,
                            (__bridge id)kCTForegroundColorAttributeName: (__bridge id)yellow,
                            (__bridge id)kCTStrokeColorAttributeName: (__bridge id)yellow,
                            (__bridge id)kCTStrokeWidthAttributeName: @(-3.0)};
    NSDictionary *backAttrs = @{(__bridge id)kCTFontAttributeName: (__bridge id)font,
                                (__bridge id)kCTForegroundColorAttributeName: (__bridge id)outline,
                                (__bridge id)kCTStrokeColorAttributeName: (__bridge id)outline,
                                (__bridge id)kCTStrokeWidthAttributeName: @(-18.0)};
    NSAttributedString *string = [[NSAttributedString alloc] initWithString:label attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
    CGFloat width = ceil(CTLineGetTypographicBounds(line, NULL, NULL, NULL) + fontSize * 0.3);
    CFRelease(line);
    text.string = string;
    back.string = [[NSAttributedString alloc] initWithString:label attributes:backAttrs];
    CFRelease(font);
    text.bounds = CGRectMake(0, 0, width, fontSize * 1.3);
    back.bounds = text.bounds;
    [back displayIfNeeded];
    [text displayIfNeeded];
}

/* The font size is tied to each control, not the host window. A packet's
 * letter occupies roughly the upper half, leaving its cost unobstructed. */
static void pvz2_badge_draw(CALayer *box, CATextLayer *text, NSString *label,
                            int index, const int *r, CGFloat sx, CGFloat sy)
{
    pvz2_badge_prime(text, label);
    CGFloat height = r[3] * sy;
    CGFloat factor = height * 0.46 / 32.0;
    CGFloat width = text.bounds.size.width * factor;
    CGFloat h = text.bounds.size.height * factor;
    CGFloat x = r[0] * sx + r[2] * sx - width - height * 0.08;
    CGFloat y = r[1] * sy;
    if (index == TL_PVZ2_BADGE_SLOTS || index == TL_PVZ2_BADGE_SLOTS + 2)
        y += height - h;
    if (index == TL_PVZ2_BADGE_SLOTS + 2) x = r[0] * sx;
    box.frame = CGRectMake(x, y, width, h);
    text.position = CGPointMake(width / 2, h / 2);
    text.affineTransform = CGAffineTransformMakeScale(factor, factor);
    CATextLayer *back = (CATextLayer *)box.sublayers.firstObject;
    back.position = CGPointMake(width / 2 + factor, h / 2 + factor);
    back.affineTransform = text.affineTransform;
    CGFloat rasterScale = box.contentsScale * MAX(1, factor);
    if (fabs(text.contentsScale - rasterScale) > 0.01) {
        text.contentsScale = rasterScale;
        back.contentsScale = rasterScale;
    }
}
