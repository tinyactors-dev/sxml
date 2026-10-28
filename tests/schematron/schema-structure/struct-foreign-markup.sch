<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" xmlns:y="urn:example:other" y:version="7">
  <x:info xmlns:x="urn:example:foreign">schema-level foreign element</x:info>
  <sch:pattern x:mark="1" xmlns:x="urn:example:foreign">
    <x:note>before the rule</x:note>
    <sch:rule context="item" x:mark="2">
      <x:note>between children</x:note>
      <sch:report test="@n = 2" x:mark="3">Item two.</sch:report>
      <x:note>after the report</x:note>
    </sch:rule>
  </sch:pattern>
</sch:schema>
