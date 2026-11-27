package java.openslrr;

import java.lang.String;

public class ModSettings
{
    public static native int getInt(String key, int def);
    public static native void setInt(String key, int v);
    public static native float getFloat(String key, float def);
    public static native void setFloat(String key, float v);
    public static native String getString(String key, String def);
    public static native void setString(String key, String v);
}
