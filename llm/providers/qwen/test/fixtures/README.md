# Image fixture

`pixel.jpg` is a generated 1×1 red JPEG, produced from an HTML canvas with
`toDataURL('image/jpeg')`. It contains no external source material. The plugin
integration test verifies that the image tool sends its exact bytes with the
JPEG media type alongside the existing PNG fixture.
