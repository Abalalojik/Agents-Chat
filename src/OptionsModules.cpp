#include "OptionsModules.h"

const std::vector<OptionsModule>& OptionsModules()
{
    static const std::vector<OptionsModule> modules = {
        {"general", "APPLICATION", "Général", OptionsView::General},
        {"connections", "INTELLIGENCES ARTIFICIELLES", "Connexions", OptionsView::Connections},
        {"models", "INTELLIGENCES ARTIFICIELLES", "Modèles", OptionsView::Models},
        {"memory", "INTELLIGENCES ARTIFICIELLES", "Mémoire commune", OptionsView::Memory},
        {"connectors", "EXTENSIONS", "Connecteurs", OptionsView::Connectors},
        {"plugins", "EXTENSIONS", "Plugins", OptionsView::Plugins},
        {"mcps", "EXTENSIONS", "MCPs", OptionsView::Mcps},
    };
    return modules;
}
