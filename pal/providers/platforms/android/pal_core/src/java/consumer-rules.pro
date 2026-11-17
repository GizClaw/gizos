# JNI exports resolve these class and native method names.
-keepclasseswithmembernames,includedescriptorclasses class com.gizclaw.gizos.pal.H2AndroidPlatform {
    native <methods>;
}
# Display.present() resolves this callback with GetMethodID.
-keepclassmembers class com.gizclaw.gizos.pal.H2AndroidPlatform$Surface {
    public void requestFrame();
}
