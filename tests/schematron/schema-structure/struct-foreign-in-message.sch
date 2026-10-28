<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[@n = 1]">
      <sch:report test="true()">Item <x:b xmlns:x="urn:example:foreign">one</x:b> found.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
