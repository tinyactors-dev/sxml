<?xml version="1.0" encoding="UTF-8"?>
<t:transform xmlns:t="http://www.w3.org/1999/XSL/Transform" version="2.0">
<!-- Purpose: Test of regex-group() with named capture groups-->

   <t:output method="xml" encoding="UTF-8" indent="no"/>

   <t:template match="doc">
      <out>
         <t:apply-templates select="date"/>
      </out>
   </t:template>

   <t:template match="date">
      <t:variable name="months" select="('January', 'February', 'March')"/>
      <t:variable name="regex"><![CDATA[\s*(?<day>[0-9]+)\s+(?<month>[A-Z][a-z]+)\s+(?<year>[0-9]+)\s*]]></t:variable>
      <t:analyze-string select="." regex="{$regex}">
         <t:matching-substring>
            <t:number value="regex-group('year')" format="0001"/>
            <t:text>-</t:text>
            <t:number value="index-of($months, regex-group('month'))" format="01"/>
            <t:text>-</t:text>
            <t:number value="regex-group('day')" format="01"/>
         </t:matching-substring>
      </t:analyze-string>
   </t:template>
</t:transform>
