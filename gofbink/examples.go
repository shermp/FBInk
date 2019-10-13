package main

import "github.com/NiLuJe/FBInk/gofbink/fbink"

func main() {
	fbCfg := &fbink.Fbinkconfig{}
	fbink.Init(fbink.FbfdAuto, fbCfg)

	fbVersion := fbink.Version()
	fbink.Cls(fbink.FbfdAuto, fbCfg)
	fbink.Print(fbink.FbfdAuto, fbVersion, fbCfg)
}
