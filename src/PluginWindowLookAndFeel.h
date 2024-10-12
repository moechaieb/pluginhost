//
// Created by Moe Chaieb on 2024-10-10.
//

#ifndef PLUGINWINDOWLOOKANDFEEL_H
#define PLUGINWINDOWLOOKANDFEEL_H
#include <juce_gui_basics/juce_gui_basics.h>

namespace timeoffaudio {
    class PluginWindowLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        void setTitleBarBackgroundColour (const juce::Colour newBackgroundColour)
        {
            getCurrentColourScheme().setUIColour (ColourScheme::UIColour::widgetBackground, newBackgroundColour);
        }

        void setTitleBarTextColour (const juce::Colour newTextColour)
        {
            getCurrentColourScheme().setUIColour (ColourScheme::UIColour::defaultText, newTextColour);
        }

        // void drawDocumentWindowTitleBar (DocumentWindow& window,
        //     Graphics& g,
        //     int w,
        //     int h,
        //     int titleSpaceX,
        //     int titleSpaceW,
        //     const Image* icon,
        //     bool drawTitleTextOnLeft) override {
        //     if (w * h == 0) return;
        //
        //     auto isActive = window.isActiveWindow();
        //
        //     g.setColour (getCurrentColourScheme().getUIColour (ColourScheme::widgetBackground));
        //     g.fillAll();
        //
        //     Font font (withDefaultMetrics (FontOptions { (float) h * 0.65f, Font::plain }));
        //     g.setFont (font);
        //
        //     auto textW = font.getStringWidth (window.getName());
        //     auto iconW = 0;
        //     auto iconH = 0;
        //
        //     if (icon != nullptr) {
        //         iconH = static_cast<int> (font.getHeight());
        //         iconW = icon->getWidth() * iconH / icon->getHeight() + 4;
        //     }
        //
        //     textW      = jmin (titleSpaceW, textW + iconW);
        //     auto textX = drawTitleTextOnLeft ? titleSpaceX : jmax (titleSpaceX, (w - textW) / 2);
        //
        //     if (textX + textW > titleSpaceX + titleSpaceW) textX = titleSpaceX + titleSpaceW - textW;
        //
        //     if (icon != nullptr) {
        //         g.setOpacity (isActive ? 1.0f : 0.6f);
        //         g.drawImageWithin (*icon, textX, (h - iconH) / 2, iconW, iconH, RectanglePlacement::centred, false);
        //         textX += iconW;
        //         textW -= iconW;
        //     }
        //
        //     if (window.isColourSpecified (DocumentWindow::textColourId)
        //         || isColourSpecified (DocumentWindow::textColourId))
        //         g.setColour (window.findColour (DocumentWindow::textColourId));
        //     else
        //         g.setColour (getCurrentColourScheme().getUIColour (ColourScheme::defaultText));
        //
        //     g.drawText (window.getName(), textX, 0, textW, h, Justification::centredLeft, true);
        // }
    };
}
#endif //PLUGINWINDOWLOOKANDFEEL_H
