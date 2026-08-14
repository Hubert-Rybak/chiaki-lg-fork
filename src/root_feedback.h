#pragma once

/*
 * Ask Homebrew Channel's optional elevated service to install/load the bundled
 * DualSense compatibility module. This is asynchronous and a no-op when the
 * TV is not rooted, the service is unavailable, or the kernel is unsupported.
 */
void root_feedback_bootstrap_async(void);

