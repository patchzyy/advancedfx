using System;
using System.Collections.Generic;
using System.Windows.Forms;

namespace AfxGui
{
    class LaunchCs2
    {
        private static void SetProcessEnvironmentVariable(Dictionary<string, string> previousValues, string key, string value)
        {
            if (!previousValues.ContainsKey(key))
            {
                previousValues[key] = Environment.GetEnvironmentVariable(key, EnvironmentVariableTarget.Process);
            }

            Environment.SetEnvironmentVariable(key, value, EnvironmentVariableTarget.Process);
        }

        private static void RestoreProcessEnvironmentVariables(Dictionary<string, string> previousValues)
        {
            foreach (KeyValuePair<string, string> entry in previousValues)
            {
                Environment.SetEnvironmentVariable(entry.Key, entry.Value, EnvironmentVariableTarget.Process);
            }
        }

        public static bool RunLauncherDialog(IWin32Window dialogOwner)
        {
            bool bOk;

            using (LaunchCs2Form frm = new LaunchCs2Form())
            {
                frm.Config = GlobalConfig.Instance.Settings.LauncherCs2;

                if (DialogResult.OK == frm.ShowDialog(dialogOwner))
                {
                    CfgLauncherCs2 cfg = frm.Config;

                    if (cfg.RememberChanges)
                    {
                        GlobalConfig.Instance.Settings.LauncherCs2 = cfg;
                    }
                    else
                    {
                        GlobalConfig.Instance.Settings.LauncherCs2.RememberChanges = cfg.RememberChanges;
                    }

                    bOk = Launch(cfg);
                }
                else
                    bOk = true;
            }

            return bOk;
        }

        private static string GetHookPath(bool isProcess64Bit)
        {
#if DEBUG
            return System.Windows.Forms.Application.StartupPath + "\\x64\\AfxHookSource2_d.dll";
#else
            return System.Windows.Forms.Application.StartupPath + "\\x64\\AfxHookSource2.dll";
#endif
        }

        public static bool Launch(CfgLauncherCs2 config)
        {
            String programPath = config.Cs2Exe;

            String cmdLine = "-steam";

            if (config.AvoidVac)
                cmdLine += " -insecure";

            if (config.GfxEnabled)
                cmdLine += " " + (config.GfxFull ? "-full" : "-sw") + " -w " + config.GfxWidth + " -h " + config.GfxHeight;

            if (config.MmcfgEnabled)
			{
				cmdLine += " -afxDisableSteamStorage";
			}

            if (0 < config.CustomLaunchOptions.Length)
                cmdLine += " " + config.CustomLaunchOptions;

            Dictionary<string, string> previousValues = new Dictionary<string, string>();
            try
            {
                if (config.MmcfgEnabled)
                {
                    SetProcessEnvironmentVariable(previousValues, "USRLOCALCSGO", config.Mmcfg);
                }

                SetProcessEnvironmentVariable(previousValues, "SteamPath", Program.SteamInstallPath);
                SetProcessEnvironmentVariable(previousValues, "SteamClientLaunch", "1");
                SetProcessEnvironmentVariable(previousValues, "SteamGameId", "730");
                SetProcessEnvironmentVariable(previousValues, "SteamAppId", "730");
                SetProcessEnvironmentVariable(previousValues, "SteamOverlayGameId", "730");

                return Loader.Load(GetHookPath, programPath, cmdLine, null, !Globals.NoGui);
            }
            finally
            {
                RestoreProcessEnvironmentVariables(previousValues);
            }
        }

    }
}
