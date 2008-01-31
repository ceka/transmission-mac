/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2005-2008 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#import "TorrentTableView.h"
#import "TorrentCell.h"
#import "Controller.h"
#import "Torrent.h"
#import "NSApplicationAdditions.h"
#import "NSMenuAdditions.h"

#define ACTION_MENU_GLOBAL_TAG 101
#define ACTION_MENU_UNLIMITED_TAG 102
#define ACTION_MENU_LIMIT_TAG 103

#define PIECE_CHANGE 0.1
#define PIECE_TIME 0.01

@interface TorrentTableView (Private)

- (BOOL) pointInControlRect: (NSPoint) point;
- (BOOL) pointInRevealRect: (NSPoint) point;
- (BOOL) pointInActionRect: (NSPoint) point;

- (BOOL) pointInProgressRect: (NSPoint) point;
- (BOOL) pointInMinimalStatusRect: (NSPoint) point;
- (void) updateFileMenu: (NSMenu *) menu forFiles: (NSArray *) files;

- (void) resizePiecesBarIncrement;

@end

@implementation TorrentTableView

- (id) initWithCoder: (NSCoder *) decoder
{
    if ((self = [super initWithCoder: decoder]))
    {
        fDefaults = [NSUserDefaults standardUserDefaults];
        
        fTorrentCell = [[TorrentCell alloc] init];
        if (![NSApp isOnLeopardOrBetter])
            [[self tableColumnWithIdentifier: @"Torrent"] setDataCell: fTorrentCell];
        
        fMouseControlRow = -1;
        fMouseRevealRow = -1;
        fMouseActionRow = -1;
        fActionPushedRow = -1;
        
        [self setDelegate: self];
        
        fPiecesBarPercent = [fDefaults boolForKey: @"PiecesBar"] ? 1.0 : 0.0;
    }
    
    return self;
}

- (void) awakeFromNib
{
    [self setGridStyleMask: NSTableViewSolidVerticalGridLineMask]; //weird redrawing issues if set to none
}

- (void) dealloc
{
    [fPiecesBarTimer invalidate];
    
    [fSelectedValues release];
    
    [fKeyStrokes release];
    [fMenuTorrent release];
    
    [fTorrentCell release];
    
    [super dealloc];
}

- (void) setTorrents: (NSArray *) torrents
{
    fTorrents = torrents;
}

- (void) setGroupIndexes: (NSIndexSet *) indexes
{
    fGroupIndexes = indexes;
}

- (BOOL) tableView: (NSTableView *) tableView isGroupRow: (NSInteger) row
{
    return [fGroupIndexes containsIndex: row];
}

- (CGFloat) tableView: (NSTableView *) tableView heightOfRow: (NSInteger) row
{
    return ![fGroupIndexes containsIndex: row] ? [self rowHeight] : GROUP_SEPARATOR_HEIGHT;
}

- (NSCell *) tableView: (NSTableView *) tableView dataCellForTableColumn: (NSTableColumn *) tableColumn row: (NSInteger) row
{
    return ![fGroupIndexes containsIndex: row] ? fTorrentCell : nil;
}

- (void) tableView: (NSTableView *) tableView willDisplayCell: (id) cell forTableColumn: (NSTableColumn *) tableColumn row: (int) row
{
    if ([fGroupIndexes containsIndex: row])
        return;
    
    [cell setRepresentedObject: [fTorrents objectAtIndex: row]];
    
    [cell setControlHover: row == fMouseControlRow];
    [cell setRevealHover: row == fMouseRevealRow];
    [cell setActionHover: row == fMouseActionRow];
    [cell setActionPushed: row == fActionPushedRow];
}

- (NSString *) tableView: (NSTableView *) tableView typeSelectStringForTableColumn: (NSTableColumn *) tableColumn row: (int) row
{
    return ![fGroupIndexes containsIndex: row] ? [[fTorrents objectAtIndex: row] name]
                                                : [[self preparedCellAtColumn: 0 row: row] stringValue];
}

- (BOOL) tableView: (NSTableView *) tableView shouldEditTableColumn: (NSTableColumn *) tableColumn row: (NSInteger) row
{
    return NO;
}

- (void) updateTrackingAreas
{
    [super updateTrackingAreas];
    
    [self removeButtonTrackingAreas];
    
    NSMutableIndexSet * indexes = [NSMutableIndexSet indexSetWithIndexesInRange: [self rowsInRect: [self visibleRect]]];
    [indexes removeIndexes: fGroupIndexes];
    if ([indexes count] == 0)
        return;
    
    NSPoint mouseLocation = [self convertPoint: [[self window] convertScreenToBase: [NSEvent mouseLocation]] fromView: nil];
    
    NSUInteger row;
    for (row = [indexes firstIndex]; row != NSNotFound; row = [indexes indexGreaterThanIndex: row])
    {
        NSDictionary * userInfo = [NSDictionary dictionaryWithObject: [NSNumber numberWithInt: row] forKey: @"Row"];
        TorrentCell * cell = (TorrentCell *)[self preparedCellAtColumn: 0 row: row];
        [cell addTrackingAreasForView: self inRect: [self frameOfCellAtColumn: 0 row: row] withUserInfo: userInfo
                mouseLocation: mouseLocation];
    }
}

- (void) removeButtonTrackingAreas
{
    fMouseControlRow = -1;
    fMouseRevealRow = -1;
    fMouseActionRow = -1;
    
    if (![NSApp isOnLeopardOrBetter])
        return;
    
    NSEnumerator * enumerator = [[self trackingAreas] objectEnumerator];
    NSTrackingArea * area;
    while ((area = [enumerator nextObject]))
    {
        if ([area owner] == self && [[area userInfo] objectForKey: @"Row"])
            [self removeTrackingArea: area];
    }
}

- (void) setControlButtonHover: (int) row
{
    fMouseControlRow = row;
    if (row >= 0)
        [self setNeedsDisplayInRect: [self rectOfRow: row]];
}

- (void) setRevealButtonHover: (int) row
{
    fMouseRevealRow = row;
    if (row >= 0)
        [self setNeedsDisplayInRect: [self rectOfRow: row]];
}

- (void) setActionButtonHover: (int) row
{
    fMouseActionRow = row;
    if (row >= 0)
        [self setNeedsDisplayInRect: [self rectOfRow: row]];
}

- (void) mouseEntered: (NSEvent *) event
{
    NSDictionary * dict = (NSDictionary *)[event userData];
    
    NSNumber * row;
    if ((row = [dict objectForKey: @"Row"]))
    {
        int rowVal = [row intValue];
        NSString * type = [dict objectForKey: @"Type"];
        if ([type isEqualToString: @"Action"])
            fMouseActionRow = rowVal;
        else if ([type isEqualToString: @"Control"])
            fMouseControlRow = rowVal;
        else
            fMouseRevealRow = rowVal;
        
        [self setNeedsDisplayInRect: [self rectOfRow: rowVal]];
    }
}

- (void) mouseExited: (NSEvent *) event
{
    NSDictionary * dict = (NSDictionary *)[event userData];
    
    NSNumber * row;
    if ((row = [dict objectForKey: @"Row"]))
    {
        NSString * type = [dict objectForKey: @"Type"];
        if ([type isEqualToString: @"Action"])
            fMouseActionRow = -1;
        else if ([type isEqualToString: @"Control"])
            fMouseControlRow = -1;
        else
            fMouseRevealRow = -1;
        
        [self setNeedsDisplayInRect: [self rectOfRow: [row intValue]]];
    }
}

- (void) tableViewSelectionIsChanging: (NSNotification *) notification
{
    if (fSelectedValues)
        [self selectValues: fSelectedValues];
}

- (void) mouseDown: (NSEvent *) event
{
    NSPoint point = [self convertPoint: [event locationInWindow] fromView: nil];
    
    BOOL pushed = [self pointInControlRect: point] || [self pointInRevealRect: point] || [self pointInActionRect: point]
                    || [self pointInProgressRect: point] || [self pointInMinimalStatusRect: point];
    
    //if pushing a button, don't change the selected rows
    if (pushed)
        fSelectedValues = [[self selectedValues] retain];
    
    [super mouseDown: event];
    
    [fSelectedValues release];
    fSelectedValues = nil;
    
    //avoid weird behavior when showing menu by doing this after mouse down
    if ([self pointInActionRect: point])
    {
        int row = [self rowAtPoint: point];
        
        fActionPushedRow = row;
        [self setNeedsDisplayInRect: [self rectOfRow: row]]; //ensure button is pushed down
        
        [self displayTorrentMenuForEvent: event];
        
        fActionPushedRow = -1;
        [self setNeedsDisplayInRect: [self rectOfRow: row]];
    }
    else if (!pushed && [event clickCount] == 2) //double click
        [fController showInfo: nil];
    else;
}

- (void) selectValues: (NSArray *) values
{
    NSMutableIndexSet * indexSet = [NSMutableIndexSet indexSet];
    
    NSEnumerator * enumerator = [values objectEnumerator];
    id object;
    while ((object = [enumerator nextObject]))
    {
        unsigned index = [fTorrents indexOfObject: object]; //works with torrents and groups
        if (index != NSNotFound)
            [indexSet addIndex: index];
    }
    
    [self selectRowIndexes: indexSet byExtendingSelection: NO];
}

- (NSArray *) selectedValues
{
    return [fTorrents objectsAtIndexes: [self selectedRowIndexes]];
}

- (NSArray *) selectedTorrents
{
    NSIndexSet * selectedIndexes = [self selectedRowIndexes];
    NSMutableIndexSet * indexSet = [NSMutableIndexSet indexSet];
    
    NSUInteger i;
    for (i = [selectedIndexes firstIndex]; i != NSNotFound; i = [selectedIndexes indexGreaterThanIndex: i])
    {
        if (![fGroupIndexes containsIndex: i])
            [indexSet addIndex: i];
        else
        {
            NSUInteger nextGroup = [fGroupIndexes indexGreaterThanIndex: i];
            if (nextGroup == NSNotFound)
                nextGroup = [fTorrents count];
            
            [indexSet addIndexesInRange: NSMakeRange(i+1, nextGroup - i - 1)];
            
             i = nextGroup - 1;
        }
    }
    [fTorrents objectsAtIndexes: indexSet];
}

- (NSMenu *) menuForEvent: (NSEvent *) event
{
    int row = [self rowAtPoint: [self convertPoint: [event locationInWindow] fromView: nil]];
    if (row >= 0)
    {
        if (![self isRowSelected: row])
            [self selectRowIndexes: [NSIndexSet indexSetWithIndex: row] byExtendingSelection: NO];
        return fContextRow;
    }
    else
    {
        [self deselectAll: self];
        return fContextNoRow;
    }
}

//make sure that the pause buttons become orange when holding down the option key
- (void) flagsChanged: (NSEvent *) event
{
    [self display];
    [super flagsChanged: event];
}

- (void) keyDown: (NSEvent *) event
{
    //this is handled by the delegate in Leopard
    if ([NSApp isOnLeopardOrBetter])
    {
        [super keyDown: event];
        return;
    }
    
    if (!fKeyStrokes)
        fKeyStrokes = [[NSMutableArray alloc] init];
    
    unichar newChar = [[event characters] characterAtIndex: 0];
    if (newChar == ' ' || [[NSCharacterSet alphanumericCharacterSet] characterIsMember: newChar]
        || [[NSCharacterSet symbolCharacterSet] characterIsMember: newChar]
        || [[NSCharacterSet punctuationCharacterSet] characterIsMember: newChar])
    {
        if ([fKeyStrokes count] > 0 && [event timestamp] - [[fKeyStrokes lastObject] timestamp] > 1.0)
            [fKeyStrokes removeAllObjects];
        [fKeyStrokes addObject: event];
    
        [self interpretKeyEvents: fKeyStrokes];
    }
    else
    {
        [fKeyStrokes removeAllObjects];
        [super keyDown: event];
    }
}

- (void) insertText: (NSString *) text
{
    //this is handled by the delegate in Leopard
    if ([NSApp isOnLeopardOrBetter])
    {
        [super insertText: text];
        return;
    }
    
    //sort torrents by name before finding closest match
    NSSortDescriptor * nameDescriptor = [[[NSSortDescriptor alloc] initWithKey: @"name" ascending: YES
                                            selector: @selector(caseInsensitiveCompare:)] autorelease];

    NSArray * tempTorrents = [fTorrents sortedArrayUsingDescriptors: [NSArray arrayWithObject: nameDescriptor]];
    
    text = [text lowercaseString];
    
    //select torrent closest to text that isn't before text alphabetically
    NSEnumerator * enumerator = [tempTorrents objectEnumerator];
    Torrent * torrent;
    while ((torrent = [enumerator nextObject]))
        if ([[[torrent name] lowercaseString] hasPrefix: text])
        {
            int row = [fTorrents indexOfObject: torrent];
            [self selectRow: row byExtendingSelection: NO];
            [self scrollRowToVisible: row];
            return;
        }
}

- (void) toggleControlForTorrent: (Torrent *) torrent
{
    if ([torrent isActive])
        [fController stopTorrents: [NSArray arrayWithObject: torrent]];
    else
    {
        if ([[NSApp currentEvent] modifierFlags] & NSAlternateKeyMask)
            [fController resumeTorrentsNoWait: [NSArray arrayWithObject: torrent]];
        else if ([torrent waitingToStart])
            [fController stopTorrents: [NSArray arrayWithObject: torrent]];
        else
            [fController resumeTorrents: [NSArray arrayWithObject: torrent]];
    }
}

- (void) displayTorrentMenuForEvent: (NSEvent *) event
{
    int row = [self rowAtPoint: [self convertPoint: [event locationInWindow] fromView: nil]];
    if (row < 0)
        return;
    
    //get and update file menu
    fMenuTorrent = [[fTorrents objectAtIndex: row] retain];
    NSMenu * fileMenu = [fMenuTorrent fileMenu];
    [self updateFileMenu: fileMenu forFiles: [fMenuTorrent fileList]];
    
    //add file menu items to action menu
    NSRange range = NSMakeRange(0, [fileMenu numberOfItems]);
    [fActionMenu appendItemsFromMenu: fileMenu atIndexes: [NSIndexSet indexSetWithIndexesInRange: range] atBottom: YES];
    
    //place menu below button
    NSRect rect = [fTorrentCell iconRectForBounds: [self frameOfCellAtColumn: 0 row: row]];
    NSPoint location = rect.origin;
    location.y += rect.size.height + 5.0;
    location = [self convertPoint: location toView: nil];
    
    NSEvent * newEvent = [NSEvent mouseEventWithType: [event type] location: location
        modifierFlags: [event modifierFlags] timestamp: [event timestamp] windowNumber: [event windowNumber]
        context: [event context] eventNumber: [event eventNumber] clickCount: [event clickCount] pressure: [event pressure]];
    
    [NSMenu popUpContextMenu: fActionMenu withEvent: newEvent forView: self];
    
    //move file menu items back to the torrent's file menu
    range.location = [fActionMenu numberOfItems] - range.length;
    [fileMenu appendItemsFromMenu: fActionMenu atIndexes: [NSIndexSet indexSetWithIndexesInRange: range] atBottom: YES];
    
    [fMenuTorrent release];
    fMenuTorrent = nil;
}

- (void) menuNeedsUpdate: (NSMenu *) menu
{
    //this method seems to be called when it shouldn't be
    if (!fMenuTorrent || ![menu supermenu])
        return;
    
    if (menu == fUploadMenu || menu == fDownloadMenu)
    {
        NSMenuItem * item;
        if ([menu numberOfItems] == 4)
        {
            const int speedLimitActionValue[] = { 0, 5, 10, 20, 30, 40, 50, 75, 100, 150, 200, 250, 500, 750, -1 };
            
            int i;
            for (i = 0; speedLimitActionValue[i] != -1; i++)
            {
                item = [[NSMenuItem alloc] initWithTitle: [NSString stringWithFormat: NSLocalizedString(@"%d KB/s",
                        "Action menu -> upload/download limit"), speedLimitActionValue[i]] action: @selector(setQuickLimit:)
                        keyEquivalent: @""];
                [item setTarget: self];
                [item setRepresentedObject: [NSNumber numberWithInt: speedLimitActionValue[i]]];
                [menu addItem: item];
                [item release];
            }
        }
        
        BOOL upload = menu == fUploadMenu;
        int mode = [fMenuTorrent speedMode: upload];
        
        item = [menu itemWithTag: ACTION_MENU_LIMIT_TAG];
        [item setState: mode == TR_SPEEDLIMIT_SINGLE ? NSOnState : NSOffState];
        [item setTitle: [NSString stringWithFormat: NSLocalizedString(@"Limit (%d KB/s)",
                    "torrent action menu -> upload/download limit"), [fMenuTorrent speedLimit: upload]]];
        
        item = [menu itemWithTag: ACTION_MENU_UNLIMITED_TAG];
        [item setState: mode == TR_SPEEDLIMIT_UNLIMITED ? NSOnState : NSOffState];
        
        item = [menu itemWithTag: ACTION_MENU_GLOBAL_TAG];
        [item setState: mode == TR_SPEEDLIMIT_GLOBAL ? NSOnState : NSOffState];
    }
    else if (menu == fRatioMenu)
    {
        NSMenuItem * item;
        if ([menu numberOfItems] == 4)
        {
            const float ratioLimitActionValue[] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, -1.0 };
            
            int i;
            for (i = 0; ratioLimitActionValue[i] != -1.0; i++)
            {
                item = [[NSMenuItem alloc] initWithTitle: [NSString stringWithFormat: @"%.2f", ratioLimitActionValue[i]]
                        action: @selector(setQuickRatio:) keyEquivalent: @""];
                [item setTarget: self];
                [item setRepresentedObject: [NSNumber numberWithFloat: ratioLimitActionValue[i]]];
                [menu addItem: item];
                [item release];
            }
        }
        
        int mode = [fMenuTorrent ratioSetting];
        
        item = [menu itemWithTag: ACTION_MENU_LIMIT_TAG];
        [item setState: mode == NSOnState ? NSOnState : NSOffState];
        [item setTitle: [NSString stringWithFormat: NSLocalizedString(@"Stop at Ratio (%.2f)", "torrent action menu -> ratio stop"),
                            [fMenuTorrent ratioLimit]]];
        
        item = [menu itemWithTag: ACTION_MENU_UNLIMITED_TAG];
        [item setState: mode == NSOffState ? NSOnState : NSOffState];
        
        item = [menu itemWithTag: ACTION_MENU_GLOBAL_TAG];
        [item setState: mode == NSMixedState ? NSOnState : NSOffState];
    }
    else  //assume the menu is part of the file list
    {
        NSMenu * supermenu = [menu supermenu];
        [self updateFileMenu: menu forFiles: [[[supermenu itemAtIndex: [supermenu indexOfItemWithSubmenu: menu]]
                                                    representedObject] objectForKey: @"Children"]];
    }
}

- (void) setQuickLimitMode: (id) sender
{
    int mode;
    switch ([sender tag])
    {
        case ACTION_MENU_UNLIMITED_TAG:
            mode = TR_SPEEDLIMIT_UNLIMITED;
            break;
        case ACTION_MENU_LIMIT_TAG:
            mode = TR_SPEEDLIMIT_SINGLE;
            break;
        case ACTION_MENU_GLOBAL_TAG:
            mode = TR_SPEEDLIMIT_GLOBAL;
            break;
        default:
            return;
    }
    
    [fMenuTorrent setSpeedMode: mode upload: [sender menu] == fUploadMenu];
    
    [[NSNotificationCenter defaultCenter] postNotificationName: @"UpdateOptions" object: nil];
}

- (void) setQuickLimit: (id) sender
{
    BOOL upload = [sender menu] == fUploadMenu;
    [fMenuTorrent setSpeedMode: TR_SPEEDLIMIT_SINGLE upload: upload];
    [fMenuTorrent setSpeedLimit: [[sender representedObject] intValue] upload: upload];
    
    [[NSNotificationCenter defaultCenter] postNotificationName: @"UpdateOptions" object: nil];
}

- (void) setQuickRatioMode: (id) sender
{
    int mode;
    switch ([sender tag])
    {
        case ACTION_MENU_UNLIMITED_TAG:
            mode = NSOffState;
            break;
        case ACTION_MENU_LIMIT_TAG:
            mode = NSOnState;
            break;
        case ACTION_MENU_GLOBAL_TAG:
            mode = NSMixedState;
            break;
        default:
            return;
    }
    
    [fMenuTorrent setRatioSetting: mode];
    
    [[NSNotificationCenter defaultCenter] postNotificationName: @"UpdateOptions" object: nil];
}

- (void) setQuickRatio: (id) sender
{
    [fMenuTorrent setRatioSetting: NSOnState];
    [fMenuTorrent setRatioLimit: [[sender representedObject] floatValue]];
    
    [[NSNotificationCenter defaultCenter] postNotificationName: @"UpdateOptions" object: nil];
}

- (void) checkFile: (id) sender
{
    NSIndexSet * indexSet = [[sender representedObject] objectForKey: @"Indexes"];
    [fMenuTorrent setFileCheckState: [sender state] != NSOnState ? NSOnState : NSOffState forIndexes: indexSet];
    
    [[NSNotificationCenter defaultCenter] postNotificationName: @"UpdateStats" object: nil];
}

- (void) togglePiecesBar
{
    [self resizePiecesBarIncrement];
    
    if (!fPiecesBarTimer)
    {
        fPiecesBarTimer = [NSTimer scheduledTimerWithTimeInterval: PIECE_TIME target: self
                            selector: @selector(resizePiecesBarIncrement) userInfo: nil repeats: YES];
        [[NSRunLoop currentRunLoop] addTimer: fPiecesBarTimer forMode: NSModalPanelRunLoopMode];
        [[NSRunLoop currentRunLoop] addTimer: fPiecesBarTimer forMode: NSEventTrackingRunLoopMode];
    }
}

- (float) piecesBarPercent
{
    return fPiecesBarPercent;
}

@end

@implementation TorrentTableView (Private)

- (BOOL) pointInControlRect: (NSPoint) point
{
    int row = [self rowAtPoint: point];
    if (row < 0 || [fGroupIndexes containsIndex: row])
        return NO;
    
    return NSPointInRect(point, [fTorrentCell controlButtonRectForBounds: [self frameOfCellAtColumn: 0 row: row]]);
}

- (BOOL) pointInRevealRect: (NSPoint) point
{
    int row = [self rowAtPoint: point];
    if (row < 0 || [fGroupIndexes containsIndex: row])
        return NO;
    
    return NSPointInRect(point, [fTorrentCell revealButtonRectForBounds: [self frameOfCellAtColumn: 0 row: row]]);
}

- (BOOL) pointInActionRect: (NSPoint) point
{
    int row = [self rowAtPoint: point];
    if (row < 0 || [fGroupIndexes containsIndex: row])
        return NO;
    
    return NSPointInRect(point, [fTorrentCell iconRectForBounds: [self frameOfCellAtColumn: 0 row: row]]);
}

- (BOOL) pointInProgressRect: (NSPoint) point
{
    int row = [self rowAtPoint: point];
    if (row < 0 || [fGroupIndexes containsIndex: row] || [fDefaults boolForKey: @"SmallView"])
        return NO;
    
    TorrentCell * cell;
    if ([NSApp isOnLeopardOrBetter])
        cell = (TorrentCell *)[self preparedCellAtColumn: 0 row: row];
    else
    {
        cell = fTorrentCell;
        [cell setRepresentedObject: [fTorrents objectAtIndex: row]];
    }
    return NSPointInRect(point, [cell progressRectForBounds: [self frameOfCellAtColumn: 0 row: row]]);
}

- (BOOL) pointInMinimalStatusRect: (NSPoint) point
{
    int row = [self rowAtPoint: point];
    if (row < 0 || [fGroupIndexes containsIndex: row] || ![fDefaults boolForKey: @"SmallView"])
        return NO;
    
    TorrentCell * cell;
    if ([NSApp isOnLeopardOrBetter])
        cell = (TorrentCell *)[self preparedCellAtColumn: 0 row: row];
    else
    {
        cell = fTorrentCell;
        [cell setRepresentedObject: [fTorrents objectAtIndex: row]];
    }
    return NSPointInRect(point, [cell minimalStatusRectForBounds: [self frameOfCellAtColumn: 0 row: row]]);
}

- (void) updateFileMenu: (NSMenu *) menu forFiles: (NSArray *) files
{
    BOOL create = [menu numberOfItems] <= 0;
    
    NSEnumerator * enumerator = [files objectEnumerator];
    NSDictionary * dict;
    NSMenuItem * item;
    while ((dict = [enumerator nextObject]))
    {
        NSString * name = [dict objectForKey: @"Name"];
        
        if (create)
        {
            item = [[NSMenuItem alloc] initWithTitle: name action: @selector(checkFile:) keyEquivalent: @""];
            
            NSImage * icon;
            if (![[dict objectForKey: @"IsFolder"] boolValue])
                icon = [[NSWorkspace sharedWorkspace] iconForFileType: [name pathExtension]];
            else
            {
                NSMenu * itemMenu = [[NSMenu alloc] initWithTitle: name];
                [itemMenu setAutoenablesItems: NO];
                [item setSubmenu: itemMenu];
                [itemMenu setDelegate: self];
                [itemMenu release];
                
                icon = [[NSWorkspace sharedWorkspace] iconForFileType: NSFileTypeForHFSTypeCode('fldr')];
            }
            
            [item setRepresentedObject: dict];
            
            [icon setScalesWhenResized: YES];
            [icon setSize: NSMakeSize(16.0, 16.0)];
            [item setImage: icon];
            
            [menu addItem: item];
            [item release];
        }
        else
            item = [menu itemWithTitle: name];
        
        NSIndexSet * indexSet = [dict objectForKey: @"Indexes"];
        [item setState: [fMenuTorrent checkForFiles: indexSet]];
        [item setEnabled: [fMenuTorrent canChangeDownloadCheckForFiles: indexSet]];
    }
}

- (void) resizePiecesBarIncrement
{
    BOOL done;
    if ([fDefaults boolForKey: @"PiecesBar"])
    {
        fPiecesBarPercent = MIN(fPiecesBarPercent + PIECE_CHANGE, 1.0);
        done = fPiecesBarPercent == 1.0;
    }
    else
    {
        fPiecesBarPercent = MAX(fPiecesBarPercent - PIECE_CHANGE, 0.0);
        done = fPiecesBarPercent == 0.0;
    }
    
    if (done)
    {
        [fPiecesBarTimer invalidate];
        fPiecesBarTimer = nil;
    }
    
    [self reloadData];
}

@end
