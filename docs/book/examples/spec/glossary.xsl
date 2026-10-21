<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output indent="yes"/>

  <!-- A dl is a flat list: dt, dd, dt, dd. Each dt starts an entry. -->
  <xsl:template match="/">
    <glossary>
      <xsl:for-each-group select="//dl/*" group-starting-with="dt">
        <entry term="{current-group()[1]}">
          <xsl:value-of select="normalize-space(current-group()[2])"/>
        </entry>
      </xsl:for-each-group>
    </glossary>
  </xsl:template>
</xsl:stylesheet>
