package org.huxerui.lib.richeditor.preview;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.HuxerUIView;
import org.huxerui.PlatformPayload;

public final class BrowserPlatform implements HuxerUIPlatformModule.WindowFactory {
    @Override
    public HuxerUIPlatformModule create(HuxerUIView window, PlatformPayload options,
            HuxerUIPlatformChannel.Events events) {
        return new HuxerUIPlatformModule() {
            @Override
            public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                    HuxerUIPlatformChannel.Result result) {
                if (!"open".equals(method)) {
                    result.fail("folio/browser-method", "Unknown browser method", PlatformPayload.nullValue());
                    return null;
                }
                Uri url = Uri.parse(arguments.requireString()).normalizeScheme();
                Intent intent = new Intent(Intent.ACTION_VIEW, url).addCategory(Intent.CATEGORY_BROWSABLE);
                boolean opened = false;
                try {
                    window.getContext().startActivity(intent);
                    opened = true;
                } catch (ActivityNotFoundException | SecurityException ignored) {
                    // The platform has no accessible handler for this destination.
                }
                result.complete(PlatformPayload.booleanValue(opened));
                return null;
            }

            @Override
            public void dispose() {}
        };
    }
}
