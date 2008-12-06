/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2008 Transmission authors and contributors
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

#import "BonjourController.h"

@implementation BonjourController

BonjourController * fDefaultController = nil;
+ (BonjourController *) defaultController
{
    if (!fDefaultController)
        fDefaultController = [[BonjourController alloc] init];
    return fDefaultController;
}

- (void) dealloc
{
    [fService release];
    [super dealloc];
}

- (void) startWithPort: (NSInteger) port
{
    [self stop];
    
    CFStringRef machineName = CSCopyMachineName();
    NSString * serviceName = [NSString stringWithFormat: @"%@: Transmission Web Interface", (NSString *)machineName];
    CFRelease(machineName);
    
    fService = [[NSNetService alloc] initWithDomain: @"local." type: @"_http._tcp." name: serviceName port: port];
    [fService setDelegate: self];
    
    [fService publish];
}

- (void) stop;
{
    [fService stop];
    [fService release];
    fService = nil;
}

- (void) netServiceWillPublish: (NSNetService *) sender
{
    NSLog(@"Will publish the Web UI service on port: %d", [sender port]);
}

- (void) netService: (NSNetService *) sender didNotPublish: (NSDictionary *) errorDict
{
    NSLog(@"Failed to publish the Web UI service on port: %d, with error: %@", [sender port], errorDict);
}

- (void) netServiceDidPublish: (NSNetService *) sender
{
    NSLog(@"Did publish the Web UI service on port: %d", [sender port]);
}

- (void) netServiceWillResolve: (NSNetService *) sender
{
    NSLog(@"Will resolve the Web UI service on port: %d", [sender port]);
}

- (void) netService: (NSNetService *) sender didNotResolve: (NSDictionary *) errorDict
{
    NSLog(@"Failed to resolve the Web UI service on port: %d, with error: %@", [sender port], errorDict);
}

- (void) netServiceDidResolveAddress: (NSNetService *) sender
{
    NSLog(@"Did resolve the Web UI service on port: %d", [sender port]);
}

@end
