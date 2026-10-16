<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns:xs="http://www.w3.org/2001/XMLSchema"
  exclude-result-prefixes="xs">

  <!-- Test of xsl:iterate; uses a parameter -->
  
  <xsl:param name="p">
    <data>
      <one/>
      <two/>
      <three/>
    </data>
  </xsl:param>

  <xsl:template match="/">
    <out>
      <xsl:iterate select="//ITEM">
        <xsl:param name="basketCost" as="xs:decimal" select="0"/>
        <xsl:param name="numbers" as="xs:string*" select="$p/data/*/local-name()"/>
        <xsl:variable name="position" select="position()"/>
        <item cost="{$basketCost}" where="{$numbers}">
          <xsl:copy-of select="TITLE"/>
        </item>
        <xsl:next-iteration>
          <xsl:with-param name="basketCost" select="$basketCost + (xs:decimal(PRICE), 0)[1]"/>
          <xsl:with-param name="numbers">
            <xsl:variable name="one" select="$p/data/*[position() - 1 = $position mod 3]"/>
            <xsl:variable name="two" select="$one/following-sibling::*[1]"/>
            <xsl:sequence select="$one/local-name() || ' ' || $two/local-name()"/>
          </xsl:with-param>
        </xsl:next-iteration>
      </xsl:iterate>
    </out>
  </xsl:template>

</xsl:stylesheet>
