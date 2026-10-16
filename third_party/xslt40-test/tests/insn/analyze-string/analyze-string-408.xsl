<?xml version="1.0" encoding="UTF-8"?>
<t:transform xmlns:t="http://www.w3.org/1999/XSL/Transform" version="2.0">
<!-- Purpose: Test of regex-groups() with 3 existing groups that have found matches in the analyzed string.-->

   <t:output method="xml" encoding="UTF-8" indent="no"/>

   <t:template match="doc">
      <out>
         <t:apply-templates select="date"/>
      </out>
   </t:template>

   <t:template match="date">
      <t:variable name="months" select="('January', 'February', 'March')"/>
      <t:analyze-string select="." regex="\s*([0-9]+)\s+([A-Z][a-z]+)\s+([0-9]+)\s*">
         <t:matching-substring>
            <t:number value="regex-groups()?3?value" format="0001"/>
            <t:text>-</t:text>
            <t:number value="index-of($months, regex-groups()?2?value)" format="01"/>
            <t:text>-</t:text>
            <t:number value="regex-groups()?1?value" format="01"/>
         </t:matching-substring>
      </t:analyze-string>
   </t:template>
</t:transform>
