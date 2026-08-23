package main

import "testing"

func TestCIdentifier(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"updatePet":   "update_pet",
		"X-Device-ID": "x_device_id",
		"42":          "value_42",
		"___":         "value",
		"struct":      "value_struct",
		"设备":          "value",
	}
	for input, expected := range cases {
		if actual := cIdentifier(input); actual != expected {
			t.Errorf("cIdentifier(%q) = %q, want %q", input, actual, expected)
		}
	}
}

func TestEscapeCStringPreservesUTF8Bytes(t *testing.T) {
	t.Parallel()
	if got, want := escapeCString("宠物\n"), "\\345\\256\\240\\347\\211\\251\\012"; got != want {
		t.Fatalf("escapeCString() = %q, want %q", got, want)
	}
}

func TestValidatePackageName(t *testing.T) {
	t.Parallel()
	for _, valid := range []string{"pets", "pet_api2"} {
		if err := validatePackageName(valid); err != nil {
			t.Errorf("validatePackageName(%q): %v", valid, err)
		}
	}
	for _, invalid := range []string{"Pet", "2pets", "pet-api", "pet__api", "pet_", "../pets"} {
		if err := validatePackageName(invalid); err == nil {
			t.Errorf("validatePackageName(%q) unexpectedly succeeded", invalid)
		}
	}
}
