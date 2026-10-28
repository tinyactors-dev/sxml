<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2">Text: <sch:dir value="rtl">مرحبا</sch:dir>, <sch:dir>plain</sch:dir>, <sch:span class="wichtig-ü">marked</sch:span>.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
